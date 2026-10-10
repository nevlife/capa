#include "host.hpp"
#include "parse_number.hpp"
#include "record.hpp"

#include <elf.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <sstream>

namespace cyra::host
{

namespace
{

void add(Report & rep, const char * id, Verdict v, std::string detail)
{
  rep.findings.push_back(Finding{id, v, std::move(detail)});
}

bool rt_policy(Policy p)
{
  return p == Policy::Fifo || p == Policy::RoundRobin;
}

std::string cpu_word(std::uint32_t c)
{
  return "cpu" + std::to_string(c);
}

std::string sysfs_cpu(std::uint32_t c, const std::string & rest)
{
  return "/sys/devices/system/cpu/cpu" + std::to_string(c) + "/" + rest;
}

long long number(const std::optional<std::string> & text, long long otherwise)
{
  if (!text) return otherwise;
  const auto v = detail::parse_number<long long>(trimmed(*text));
  return v ? *v : otherwise;
}

// "always [madvise] never" -> "madvise"
std::string bracketed(const std::string & text)
{
  const auto open = text.find('[');
  const auto close = text.find(']', open);
  if (open == std::string::npos || close == std::string::npos) return trimmed(text);
  return text.substr(open + 1, close - open - 1);
}

std::string parenthesized(const std::string & text)
{
  const auto open = text.find('(');
  const auto close = text.find(')', open);
  if (open == std::string::npos || close == std::string::npos) return {};
  return text.substr(open + 1, close - open - 1);
}

bool preempt_rt(const Fs & fs)
{
  const auto realtime = fs.read("/sys/kernel/realtime");
  const auto version = fs.read("/proc/version");
  return (realtime && trimmed(*realtime) == "1") ||
         (version && version->find("PREEMPT_RT") != std::string::npos);
}

std::pair<int, int> kernel_version(const Fs & fs)
{
  const auto w = words(fs.read("/proc/version").value_or(""));
  if (w.size() < 3) return {0, 0};
  const auto dot = w[2].find('.');
  const auto end = w[2].find_first_not_of("0123456789", dot + 1);
  return {
    detail::parse_number<int>(std::string_view(w[2]).substr(0, dot)).value_or(0),
    detail::parse_number<int>(std::string_view(w[2]).substr(dot + 1, end - dot - 1)).value_or(0)};
}

std::string proc_dir(int tid)
{
  return tid == 0 ? "/proc/self" : "/proc/" + std::to_string(tid);
}

Cpus online(const Fs & fs)
{
  return parse_cpu_list(fs.read("/sys/devices/system/cpu/online").value_or(""));
}

bool running(const std::vector<Task> & procs, const std::string & comm)
{
  return std::any_of(procs.begin(), procs.end(), [&](const Task & t) {
    return !t.kernel && t.comm == comm.substr(0, 15);
  });
}

std::string layers(const Fs & fs)
{
  std::vector<std::string> out;
  const auto status = fs.read("/proc/self/status").value_or("");
  const auto mode = field(status, "Seccomp").value_or("0");
  if (mode != "0") {
    out.push_back(
      "seccomp mode " + mode + " with " + field(status, "Seccomp_filters").value_or("?") +
      " filter(s) (systemd RestrictRealtime=yes refuses RT this way)");
  }
  const auto label = trimmed(fs.read("/proc/self/attr/current").value_or(""));
  if (!label.empty() && label != "unconfined") out.push_back("LSM label " + label);
  std::istringstream mounts(fs.read("/proc/self/mountinfo").value_or(""));
  std::string line;
  while (std::getline(mounts, line)) {
    const auto at = line.find(" - proc ");
    const auto hide = at == std::string::npos ? at : line.find("hidepid=", at);
    if (hide != std::string::npos) {
      out.push_back(
        "/proc mounted with " + line.substr(hide, line.find_first_of(", ", hide) - hide));
    }
  }
  return list_some(out, out.size());
}

void permission(Report & rep, const Fs & fs, const Options & opts)
{
  const auto status = fs.read("/proc/self/status");
  const auto cap_eff = status ? field(*status, "CapEff") : std::nullopt;
  const auto limits = fs.read("/proc/self/limits").value_or("");
  std::string soft = "unreadable";
  bool covers = false;
  std::istringstream lines(limits);
  std::string line;
  while (std::getline(lines, line)) {
    if (line.rfind("Max realtime priority", 0) != 0) continue;
    const auto w = words(line);
    soft = w.size() > 3 ? w[3] : soft;
    covers = soft == "unlimited" || number(soft, 0) >= opts.priority;
  }
  if (cap_eff && ((std::strtoull(cap_eff->c_str(), nullptr, 16) >> 23) & 1) != 0) {
    add(rep, "policy-permission", Verdict::Ok, "CAP_SYS_NICE is in CapEff");
  } else if (covers) {
    add(
      rep, "policy-permission", Verdict::Ok,
      "RLIMIT_RTPRIO soft limit " + soft + " covers requested " + std::to_string(opts.priority));
  } else if (!cap_eff) {
    add(rep, "policy-permission", Verdict::Unknown, "cannot read CapEff from /proc/self/status");
  } else {
    const std::string blocking = layers(fs);
    add(
      rep, "policy-permission", Verdict::Fail,
      "no CAP_SYS_NICE and RLIMIT_RTPRIO soft limit " + soft + " < requested " +
        std::to_string(opts.priority) +
        ": raise the rtprio ulimit (LimitRTPRIO=, limits.conf) or grant CAP_SYS_NICE" +
        (blocking.empty() ? "" : "; also in the way: " + blocking));
  }
}

void secure_exec(Report & rep, const Fs & fs, int tid)
{
  const auto auxv = fs.read(proc_dir(tid) + "/auxv");
  if (!auxv) {
    add(rep, "secure-exec", Verdict::Unknown, "cannot read " + proc_dir(tid) + "/auxv");
    return;
  }
  const std::size_t pairs = auxv->size() / (2 * sizeof(std::uint64_t));
  for (std::size_t i = 0; i < pairs; ++i) {
    std::uint64_t kv[2];
    std::memcpy(kv, &(*auxv)[i * sizeof kv], sizeof kv);
    if (kv[0] == AT_SECURE && kv[1] != 0) {
      add(
        rep, "secure-exec", Verdict::Warn,
        "the process runs secure-exec (file capabilities or setuid), so ld.so dropped "
        "LD_PRELOAD and LD_LIBRARY_PATH: preloaded tracers are silently gone. Grant RT with "
        "RLIMIT_RTPRIO (LimitRTPRIO=, limits.conf) rather than setcap cap_sys_nice");
      return;
    }
  }
  add(rep, "secure-exec", Verdict::Ok, "not secure-exec: LD_PRELOAD is honored");
}

bool elf_binds_now(const std::string & file)
{
  const int fd = ::open(file.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return true;
  Elf64_Ehdr eh{};
  bool now = true;
  if (
    ::pread(fd, &eh, sizeof eh, 0) == sizeof eh && std::memcmp(eh.e_ident, ELFMAG, SELFMAG) == 0 &&
    eh.e_ident[EI_CLASS] == ELFCLASS64) {
    for (int i = 0; i < eh.e_phnum; ++i) {
      Elf64_Phdr ph{};
      const auto at = static_cast<off_t>(eh.e_phoff + i * sizeof ph);
      if (::pread(fd, &ph, sizeof ph, at) != sizeof ph || ph.p_type != PT_DYNAMIC) continue;
      now = false;
      for (std::uint64_t off = 0; off + sizeof(Elf64_Dyn) <= ph.p_filesz;
           off += sizeof(Elf64_Dyn)) {
        Elf64_Dyn d{};
        if (::pread(fd, &d, sizeof d, static_cast<off_t>(ph.p_offset + off)) != sizeof d) break;
        if (d.d_tag == DT_NULL) break;
        std::uint64_t val = 0;
        std::memcpy(&val, &d.d_un, sizeof val);
        now = now || d.d_tag == DT_BIND_NOW || (d.d_tag == DT_FLAGS && (val & DF_BIND_NOW) != 0) ||
              (d.d_tag == DT_FLAGS_1 && (val & DF_1_NOW) != 0);
      }
    }
  }
  ::close(fd);
  return now;
}

void bind_now(Report & rep, const Fs & fs, int tid)
{
  const auto environ = fs.read(proc_dir(tid) + "/environ");
  const auto maps = fs.read(proc_dir(tid) + "/maps");
  if (!environ || !maps) {
    add(rep, "bind-now", Verdict::Unknown, "cannot read " + proc_dir(tid) + "/environ and maps");
    return;
  }
  std::istringstream vars(*environ);
  std::string var;
  while (std::getline(vars, var, '\0')) {
    if (var.rfind("LD_BIND_NOW=", 0) == 0 && var.size() > 12) {
      add(rep, "bind-now", Verdict::Ok, "LD_BIND_NOW is set: every symbol resolves at load");
      return;
    }
  }
  std::set<std::string> objects;
  std::istringstream lines(*maps);
  std::string line;
  while (std::getline(lines, line)) {
    const auto slash = line.find(" /");
    if (slash != std::string::npos) objects.insert(line.substr(slash + 1));
  }
  std::vector<std::string> lazy;
  for (const auto & o : objects) {
    if (!elf_binds_now(fs.path(o))) lazy.push_back(o.substr(o.rfind('/') + 1));
  }
  if (lazy.empty()) {
    add(rep, "bind-now", Verdict::Ok, "every mapped object is linked with BIND_NOW");
    return;
  }
  add(
    rep, "bind-now", Verdict::Warn,
    std::to_string(lazy.size()) + " mapped object(s) bind lazily (" + list_some(lazy, 3) +
      "): the first call through each PLT entry runs the dynamic linker on the RT path. Set "
      "LD_BIND_NOW=1 or link with -z now");
}

void rt_throttle(Report & rep, const Fs & fs, const Options & opts)
{
  const auto runtime = fs.read("/proc/sys/kernel/sched_rt_runtime_us");
  if (!runtime) {
    add(rep, "rt-throttle", Verdict::Unknown, "cannot read /proc/sys/kernel/sched_rt_runtime_us");
    return;
  }
  const Cpus cpus = opts.cpus.empty() ? online(fs) : opts.cpus;
  std::vector<std::string> served;
  bool fair_known = fs.exists("/sys/kernel/debug/sched/fair_server");
  for (auto c : cpus) {
    const std::string d = "/sys/kernel/debug/sched/fair_server/" + cpu_word(c);
    const long long run = number(fs.read(d + "/runtime"), 0);
    if (run > 0) {
      served.push_back(
        cpu_word(c) + " " + std::to_string(run / 1000000) + " ms per " +
        std::to_string(number(fs.read(d + "/period"), 0) / 1000000) + " ms");
    }
  }
  const auto [major, minor] = kernel_version(fs);
  const bool has_fair_server = major > 6 || (major == 6 && minor >= 12);
  const std::string fair =
    !served.empty() ? "the fair deadline server still takes " + list_some(served, 3) +
                        " from RT tasks whenever SCHED_OTHER tasks starve there"
    : fair_known || !has_fair_server ? std::string()
                                     : "the fair deadline server (6.12+) is in debugfs, which "
                                       "this user cannot read (root only, or lockdown)";
  const long long rt_us = number(runtime, 0);
  if (rt_us >= 0) {
    add(
      rep, "rt-throttle", Verdict::Warn,
      "sched_rt_runtime_us=" + std::to_string(rt_us) + " of period " +
        trimmed(fs.read("/proc/sys/kernel/sched_rt_period_us").value_or("1000000")) +
        ": the kernel takes the CPU from RT tasks for the rest of every period, so a busy RT "
        "thread is preempted regardless of priority; hard RT wants sched_rt_runtime_us=-1" +
        (fair.empty() ? "" : "; " + fair));
  } else if (!served.empty()) {
    add(
      rep, "rt-throttle", Verdict::Warn,
      "sched_rt_runtime_us=-1, but " + fair +
        ". Set its runtime to 0 in debugfs for the stage cpus once nothing there needs it");
  } else if (!fair.empty()) {
    add(rep, "rt-throttle", Verdict::Unknown, "sched_rt_runtime_us=-1, but " + fair);
  } else {
    add(rep, "rt-throttle", Verdict::Ok, "sched_rt_runtime_us=-1 and no fair server runtime");
  }
}

void priority_order(Report & rep, const Options & opts, int control)
{
  if (control <= 0) {
    add(
      rep, "priority-order", Verdict::Unknown,
      "control loop priority not declared; pass it to preflight() so transport can be checked "
      "to stay strictly below it");
  } else if (opts.priority < control) {
    add(
      rep, "priority-order", Verdict::Ok,
      "transport " + std::to_string(opts.priority) + " stays below control loop " +
        std::to_string(control));
  } else {
    add(
      rep, "priority-order", Verdict::Fail,
      "transport priority " + std::to_string(opts.priority) + " >= control loop " +
        std::to_string(control) +
        ": transport must never preempt or starve control; set it strictly lower");
  }
}

void priority_band(Report & rep, const Options & opts)
{
  if (opts.priority == 99) {
    add(
      rep, "priority-band", Verdict::Warn,
      "priority 99 is where the kernel's own watchdog and stopper work runs; a stage there "
      "delays them. Use 80..89 for main RT tasks and 85..94 for the IRQ threads that feed them");
  } else {
    add(rep, "priority-band", Verdict::Ok, "priority " + std::to_string(opts.priority));
  }
}

void cpu_online(Report & rep, const Fs & fs, const Options & opts)
{
  const auto text = fs.read("/sys/devices/system/cpu/online");
  if (!text) {
    add(rep, "cpu-online", Verdict::Unknown, "cannot read /sys/devices/system/cpu/online");
    return;
  }
  Cpus off;
  for (auto c : opts.cpus) {
    if (!contains(parse_cpu_list(*text), c)) off.push_back(c);
  }
  if (off.empty()) {
    add(rep, "cpu-online", Verdict::Ok, "requested cpus online");
  } else {
    add(
      rep, "cpu-online", Verdict::Fail,
      "cpu " + join(off) + " not online (online: " + trimmed(*text) + ")");
  }
}

void cpu_isolation(Report & rep, const Fs & fs, const Options & opts)
{
  const auto boot = fs.read("/sys/devices/system/cpu/isolated");
  const auto part = fs.read("/sys/fs/cgroup/cpuset.cpus.isolated");
  if (!boot && !part) {
    add(rep, "cpu-isolation", Verdict::Unknown, "cannot read /sys/devices/system/cpu/isolated");
    return;
  }
  const Cpus isolated = isolated_cpus(fs);
  Cpus shared;
  for (auto c : opts.cpus) {
    if (!contains(isolated, c)) shared.push_back(c);
  }
  const std::string sources =
    "isolcpus: " + trimmed(boot.value_or("?")) +
    ", isolated cpuset partitions: " + trimmed(part.value_or("none")) +
    ", nohz_full: " + trimmed(fs.read("/sys/devices/system/cpu/nohz_full").value_or("?"));
  if (shared.empty()) {
    add(rep, "cpu-isolation", Verdict::Ok, "requested cpus are isolated (" + sources + ")");
  } else {
    add(
      rep, "cpu-isolation", Verdict::Warn,
      "cpu " + join(shared) +
        " not isolated: pinning removes migration but the core still runs other tasks; isolate "
        "with isolcpus/nohz_full or an isolated cpuset partition (" +
        sources + ")");
  }
}

void cpuset(Report & rep, const Fs & fs, int tid, const Options & opts)
{
  const std::string dir = cgroup_dir(fs, tid);
  if (dir.empty()) {
    add(
      rep, "cpuset", Verdict::Unknown,
      "no cgroup v2 line in " + proc_dir(tid) + "/cgroup (a cgroup v1 or hybrid host)");
    return;
  }
  std::optional<std::string> effective;
  std::string where;
  std::vector<std::string> invalid;
  for (const auto & d : cgroup_chain(dir)) {
    if (!effective) {
      effective = fs.read(d + "/cpuset.cpus.effective");
      where = d;
    }
    const auto p = fs.read(d + "/cpuset.cpus.partition");
    if (p && p->find("invalid") != std::string::npos) invalid.push_back(d + ": " + trimmed(*p));
  }
  if (!effective) {
    add(rep, "cpuset", Verdict::Unknown, "no cpuset.cpus.effective above " + dir);
    return;
  }
  Cpus outside;
  for (auto c : opts.cpus) {
    if (!contains(parse_cpu_list(*effective), c)) outside.push_back(c);
  }
  if (!outside.empty()) {
    add(
      rep, "cpuset", Verdict::Fail,
      "cpu " + join(outside) + " is outside cpuset.cpus.effective " + trimmed(*effective) + " of " +
        where + " (a container, or systemd AllowedCPUs): the kernel narrows the mask");
  } else if (!invalid.empty()) {
    add(
      rep, "cpuset", Verdict::Warn,
      "an invalid cpuset partition does not isolate what it names: " + list_some(invalid, 3) +
        ". Re-run preflight after any partition change: kernels before 7.0 let kthreads into a "
        "partition made at run time");
  } else {
    add(
      rep, "cpuset", Verdict::Ok,
      "requested cpus within cpuset.cpus.effective " + trimmed(*effective) + " of " + where);
  }
}

void cpu_governor(Report & rep, const Fs & fs, const Options & opts)
{
  std::vector<std::string> slow;
  bool missing = false;
  for (auto c : opts.cpus) {
    const auto governor = fs.read(sysfs_cpu(c, "cpufreq/scaling_governor"));
    if (!governor) {
      missing = true;
    } else if (trimmed(*governor) != "performance") {
      slow.push_back(cpu_word(c) + "=" + trimmed(*governor));
    }
  }
  if (!slow.empty()) {
    add(
      rep, "cpu-governor", Verdict::Warn,
      list_some(slow, 4) + ": frequency ramp adds wakeup latency; set performance on RT cores");
  } else if (missing) {
    add(rep, "cpu-governor", Verdict::Unknown, "no cpufreq on a requested cpu (VM or fixed clock)");
  } else {
    add(rep, "cpu-governor", Verdict::Ok, "governor performance on requested cpus");
  }
}

void cpufreq(Report & rep, const Fs & fs, const Options & opts)
{
  std::vector<std::string> facts;
  std::vector<std::string> slow;
  for (auto c : opts.cpus) {
    const auto driver = fs.read(sysfs_cpu(c, "cpufreq/scaling_driver"));
    if (!driver) continue;
    const auto epp = fs.read(sysfs_cpu(c, "cpufreq/energy_performance_preference"));
    const long long min = number(fs.read(sysfs_cpu(c, "cpufreq/scaling_min_freq")), 0);
    const long long max = number(fs.read(sysfs_cpu(c, "cpufreq/cpuinfo_max_freq")), 0);
    facts.push_back(
      cpu_word(c) + " " + trimmed(*driver) + (epp ? " EPP " + trimmed(*epp) : "") + ", min " +
      std::to_string(min / 1000) + " of " + std::to_string(max / 1000) + " MHz, policy cpus " +
      trimmed(fs.read(sysfs_cpu(c, "cpufreq/related_cpus")).value_or("?")) + ", transition " +
      trimmed(fs.read(sysfs_cpu(c, "cpufreq/cpuinfo_transition_latency")).value_or("?")) + " ns");
    if (epp && trimmed(*epp) != "performance") slow.push_back(cpu_word(c));
  }
  if (facts.empty()) {
    add(rep, "cpufreq", Verdict::Unknown, "no cpufreq on the requested cpus");
    return;
  }
  const auto no_turbo = fs.read("/sys/devices/system/cpu/intel_pstate/no_turbo");
  const auto boost = fs.read("/sys/devices/system/cpu/cpufreq/boost");
  const std::string turbo = no_turbo ? (trimmed(*no_turbo) == "1" ? "; turbo off" : "; turbo on")
                            : boost  ? (trimmed(*boost) == "1" ? "; boost on" : "; boost off")
                                     : "";
  if (!slow.empty()) {
    add(
      rep, "cpufreq", Verdict::Warn,
      "energy_performance_preference is not performance on " + join(opts.cpus) +
        ": in active intel_pstate or amd-pstate mode EPP, not the governor, decides how fast the "
        "clock ramps (" +
        list_some(facts, 2) + turbo + ")");
  } else {
    add(rep, "cpufreq", Verdict::Ok, list_some(facts, 2) + turbo);
  }
}

void rcu_offload(Report & rep, const Fs & fs, const Options & opts)
{
  const auto cmdline = fs.read("/proc/cmdline");
  if (!cmdline) {
    add(rep, "rcu-offload", Verdict::Unknown, "cannot read /proc/cmdline");
    return;
  }
  const auto nocbs = boot_param(*cmdline, "rcu_nocbs");
  const Cpus offloaded = nocbs ? parse_cpu_list(*nocbs) : Cpus{};
  const Cpus nohz = parse_cpu_list(fs.read("/sys/devices/system/cpu/nohz_full").value_or(""));
  Cpus onboard;
  for (auto c : opts.cpus) {
    if (!contains(offloaded, c) && !contains(nohz, c)) onboard.push_back(c);
  }
  if (onboard.empty()) {
    add(rep, "rcu-offload", Verdict::Ok, "requested cpus offload RCU callbacks");
    return;
  }
  add(
    rep, "rcu-offload", Verdict::Warn,
    "cpu " + join(onboard) +
      " runs its own RCU callbacks: deferred kernel frees fire on the RT core at times nothing "
      "bounds. Add rcu_nocbs (or nohz_full, which implies it) for these cpus (rcu_nocbs: " +
      nocbs.value_or("unset") + ")");
}

// The handler's name is a subdirectory of /proc/irq/<n>.
std::string irq_name(const Fs & fs, const std::string & irq)
{
  for (const auto & e : fs.list("/proc/irq/" + irq)) {
    if (fs.exists("/proc/irq/" + irq + "/" + e + "/.")) return e;
  }
  return {};
}

void irq_affinity(Report & rep, const Fs & fs, const Options & opts)
{
  const auto irqs = fs.list("/proc/irq");
  if (irqs.empty()) {
    add(rep, "irq-affinity", Verdict::Unknown, "cannot read /proc/irq");
    return;
  }
  std::vector<std::string> named;
  std::vector<std::string> bare;
  std::vector<std::string> managed;
  const bool debugfs = fs.exists("/sys/kernel/debug/irq/irqs");
  for (const auto & irq : irqs) {
    if (std::isdigit(static_cast<unsigned char>(irq[0])) == 0) continue;
    const std::string dir = "/proc/irq/" + irq;
    auto list = fs.read(dir + "/effective_affinity_list");
    if (!list || trimmed(*list).empty()) list = fs.read(dir + "/smp_affinity_list");
    if (!list || !overlap(opts.cpus, parse_cpu_list(*list))) continue;
    const std::string name = irq_name(fs, irq);
    (name.empty() ? bare : named).push_back("irq " + irq + (name.empty() ? "" : " " + name));
    const auto state = fs.read("/sys/kernel/debug/irq/irqs/" + irq);
    if (state && state->find("AFFINITY_MANAGED") != std::string::npos) {
      managed.push_back("irq " + irq + (name.empty() ? "" : " " + name));
    }
  }
  named.insert(named.end(), bare.begin(), bare.end());
  if (named.empty()) {
    add(rep, "irq-affinity", Verdict::Ok, "no interrupt is routed to the requested cpus");
  } else {
    add(
      rep, "irq-affinity", Verdict::Warn,
      std::to_string(named.size()) + " interrupt(s) can land on the requested cpus (" +
        list_some(named, 3) +
        "): a handler preempts every task on its cpu regardless of RT priority. Steer them away "
        "with /proc/irq/<n>/smp_affinity_list, confirm with effective_affinity_list, and put "
        "them back when the chain stops; a write can fail with EIO (no set_affinity in the "
        "chip) or ENOSPC (x86 vectors used up)");
  }
  if (!managed.empty()) {
    add(
      rep, "irq-managed", Verdict::Warn,
      list_some(managed, 3) +
        ": managed interrupts cannot be moved from user space; boot with "
        "isolcpus=managed_irq,<cpus> to keep them off");
  } else if (debugfs || named.empty()) {
    add(rep, "irq-managed", Verdict::Ok, "no managed interrupt on the requested cpus");
  } else {
    add(
      rep, "irq-managed", Verdict::Unknown,
      "which interrupts are managed is in debugfs (irq/irqs), which this user cannot read");
  }
}

void irq_default(Report & rep, const Fs & fs, const Options & opts)
{
  const auto mask = fs.read("/proc/irq/default_smp_affinity");
  if (!mask) {
    add(
      rep, "irq-default-affinity", Verdict::Unknown, "cannot read /proc/irq/default_smp_affinity");
    return;
  }
  Cpus hit;
  for (auto c : opts.cpus) {
    if (contains(parse_cpu_mask(*mask), c)) hit.push_back(c);
  }
  if (hit.empty()) {
    add(
      rep, "irq-default-affinity", Verdict::Ok, "interrupts set up later avoid the requested cpus");
  } else {
    add(
      rep, "irq-default-affinity", Verdict::Warn,
      "/proc/irq/default_smp_affinity " + trimmed(*mask) + " includes cpu " + join(hit) +
        ": an interrupt set up later (a hotplugged USB camera) starts there");
  }
}

void irqbalance(Report & rep, const std::vector<Task> & procs)
{
  if (running(procs, "irqbalance")) {
    add(
      rep, "irqbalance", Verdict::Warn,
      "irqbalance runs and rewrites interrupt affinity about every 10 s, undoing manual steering; "
      "ban the stage cpus with IRQBALANCE_BANNED_CPULIST or stop it");
  } else {
    add(rep, "irqbalance", Verdict::Ok, "irqbalance is not running");
  }
}

void cpu_idle(Report & rep, const Fs & fs, const Options & opts)
{
  std::vector<std::string> deep;
  bool any = false;
  for (auto c : opts.cpus) {
    const auto qos_text =
      trimmed(fs.read(sysfs_cpu(c, "power/pm_qos_resume_latency_us")).value_or("0"));
    const long long qos = qos_text == "n/a" ? 0 : number(qos_text, 0);
    const bool polls_only = qos_text == "n/a";
    for (const auto & s : fs.list(sysfs_cpu(c, "cpuidle"))) {
      if (s.rfind("state", 0) != 0) continue;
      any = true;
      const std::string d = sysfs_cpu(c, "cpuidle/" + s + "/");
      const long long latency = number(fs.read(d + "latency"), 0);
      const bool off = number(fs.read(d + "disable"), 0) != 0;
      const bool allowed = !polls_only && (qos == 0 || latency <= qos);
      if (!off && allowed && latency > 50) {
        deep.push_back(
          cpu_word(c) + " " + trimmed(fs.read(d + "name").value_or(s)) + " (exit " +
          std::to_string(latency) + " us)");
      }
    }
  }
  if (!any) {
    add(rep, "cpu-idle", Verdict::Unknown, "no cpuidle states on the requested cpus");
  } else if (deep.empty()) {
    add(rep, "cpu-idle", Verdict::Ok, "no enabled idle state exits slower than 50 us");
  } else {
    add(
      rep, "cpu-idle", Verdict::Warn,
      "idle states deeper than 50 us are allowed: " + list_some(deep, 3) +
        ". Cap them with power/pm_qos_resume_latency_us (10..50 us), cpuidle/stateN/disable, or "
        "an open /dev/cpu_dma_latency; not idle=poll, which brings thermal throttling on Jetson");
  }
}

void workqueue(Report & rep, const Fs & fs, const Options & opts)
{
  const auto mask = fs.read("/sys/devices/virtual/workqueue/cpumask");
  if (!mask) {
    add(rep, "workqueue", Verdict::Unknown, "cannot read /sys/devices/virtual/workqueue/cpumask");
    return;
  }
  Cpus hit;
  for (auto c : opts.cpus) {
    if (contains(parse_cpu_mask(*mask), c)) hit.push_back(c);
  }
  if (hit.empty()) {
    add(rep, "workqueue", Verdict::Ok, "unbound workqueues avoid the requested cpus");
  } else {
    add(
      rep, "workqueue", Verdict::Warn,
      "workqueue cpumask " + trimmed(*mask) + " includes cpu " + join(hit) +
        ": unbound kernel work runs there; write a mask without them");
  }
}

void kthreads(Report & rep, const std::vector<Task> & procs, const Options & opts)
{
  std::vector<std::string> hit;
  for (const auto & t : procs) {
    if (t.kernel && t.cpus.size() > 1 && overlap(t.cpus, opts.cpus)) hit.push_back(t.comm);
  }
  if (hit.empty()) {
    add(
      rep, "kthread-affinity", Verdict::Ok,
      "no unbound kernel thread may run on the requested cpus");
  } else {
    add(
      rep, "kthread-affinity", Verdict::Warn,
      std::to_string(hit.size()) + " unbound kernel thread(s) may run on the requested cpus (" +
        list_some(hit, 3) + ")");
  }
}

void smt(Report & rep, const Fs & fs, const Options & opts)
{
  const Cpus isolated = isolated_cpus(fs);
  std::vector<std::string> open;
  bool read_any = false;
  for (auto c : opts.cpus) {
    const auto sib = fs.read(sysfs_cpu(c, "topology/thread_siblings_list"));
    if (!sib) continue;
    read_any = true;
    for (auto s : parse_cpu_list(*sib)) {
      if (s != c && !contains(opts.cpus, s) && !contains(isolated, s)) {
        open.push_back(cpu_word(c) + "/" + cpu_word(s));
      }
    }
  }
  if (!read_any) {
    add(rep, "smt", Verdict::Unknown, "cannot read topology/thread_siblings_list");
  } else if (open.empty()) {
    add(rep, "smt", Verdict::Ok, "no SMT sibling of the requested cpus runs other work");
  } else {
    add(
      rep, "smt", Verdict::Warn,
      "SMT siblings not isolated: " + list_some(open, 3) +
        ". A task on the sibling shares the core's pipeline and caches");
  }
}

void capacity(Report & rep, const Fs & fs, const Options & opts)
{
  long long top = 0;
  for (auto c : online(fs)) top = std::max(top, number(fs.read(sysfs_cpu(c, "cpu_capacity")), 0));
  if (top == 0) {
    add(rep, "cpu-capacity", Verdict::Unknown, "no cpu_capacity in sysfs");
    return;
  }
  std::vector<std::string> low;
  for (auto c : opts.cpus) {
    const long long cap = number(fs.read(sysfs_cpu(c, "cpu_capacity")), top);
    if (cap < top) low.push_back(cpu_word(c) + " " + std::to_string(cap));
  }
  if (low.empty()) {
    add(rep, "cpu-capacity", Verdict::Ok, "requested cpus have the highest capacity");
  } else {
    add(
      rep, "cpu-capacity", Verdict::Warn,
      "lower-capacity cores requested (" + list_some(low, 3) + " of " + std::to_string(top) +
        "): an efficiency core runs the same work slower");
  }
}

void softlockup(Report & rep, const Fs & fs, const Options & opts)
{
  const auto watchdog = fs.read("/proc/sys/kernel/watchdog");
  const auto soft = fs.read("/proc/sys/kernel/soft_watchdog");
  const auto mask = fs.read("/proc/sys/kernel/watchdog_cpumask");
  if (!watchdog || !mask) {
    add(rep, "softlockup-watchdog", Verdict::Unknown, "cannot read /proc/sys/kernel/watchdog");
    return;
  }
  Cpus hit;
  for (auto c : opts.cpus) {
    if (contains(parse_cpu_list(*mask), c)) hit.push_back(c);
  }
  if (trimmed(*watchdog) == "0" || trimmed(soft.value_or("1")) == "0" || hit.empty()) {
    add(rep, "softlockup-watchdog", Verdict::Ok, "the softlockup watchdog leaves the cpus alone");
  } else {
    add(
      rep, "softlockup-watchdog", Verdict::Warn,
      "cpu " + join(hit) +
        " is in watchdog_cpumask: a 4 s hrtimer there wakes the stopper thread above FIFO 99. "
        "The default mask follows nohz_full, so isolcpus alone leaves the cpu in it");
  }
}

void kernel_preemption(Report & rep, const Fs & fs)
{
  if (preempt_rt(fs)) {
    add(rep, "kernel-preemption", Verdict::Ok, "PREEMPT_RT kernel");
  } else if (fs.read("/proc/version")) {
    add(
      rep, "kernel-preemption", Verdict::Warn,
      "not a PREEMPT_RT kernel: in-kernel sections are not preemptible, so worst-case latency is "
      "unbounded by userspace scheduling alone");
  } else {
    add(rep, "kernel-preemption", Verdict::Unknown, "cannot read /proc/version");
  }
}

void preempt_mode(Report & rep, const Fs & fs, const Options & opts)
{
  if (preempt_rt(fs)) {
    add(rep, "preempt-mode", Verdict::Ok, "PREEMPT_RT preempts fully");
    return;
  }
  const std::string version = fs.read("/proc/version").value_or("");
  std::string mode;
  if (version.find("PREEMPT_DYNAMIC") != std::string::npos) {
    const auto text = fs.read("/sys/kernel/debug/sched/preempt");
    if (!text) {
      add(
        rep, "preempt-mode", Verdict::Unknown,
        "the dynamic preemption mode is in debugfs sched/preempt, which this user cannot read");
      return;
    }
    mode = parenthesized(*text);
  } else {
    mode = version.find(" PREEMPT ") != std::string::npos ? "full" : "none or voluntary";
  }
  if (mode == "full") {
    add(rep, "preempt-mode", Verdict::Ok, "preempt=full");
  } else if (mode == "lazy" && rt_policy(opts.policy)) {
    add(rep, "preempt-mode", Verdict::Ok, "preempt=lazy, which still preempts at once for RT");
  } else if (mode == "lazy") {
    add(
      rep, "preempt-mode", Verdict::Warn,
      "preempt=lazy: a woken SCHED_OTHER thread waits for the next tick to preempt");
  } else {
    add(
      rep, "preempt-mode", Verdict::Warn,
      "preempt=" + mode + ": kernel code runs on to its next preemption point; boot preempt=full");
  }
}

void sched_ext(Report & rep, const Fs & fs, const Options & opts)
{
  const auto state = fs.read("/sys/kernel/sched_ext/state");
  if (!state || trimmed(*state) == "disabled") {
    add(
      rep, "sched-ext", Verdict::Ok,
      state ? "no BPF scheduler loaded" : "no sched_ext in this kernel");
    return;
  }
  const std::string ops = trimmed(fs.read("/sys/kernel/sched_ext/root/ops").value_or("?"));
  if (rt_policy(opts.policy) && opts.children == Children::Inherit) {
    add(rep, "sched-ext", Verdict::Ok, "BPF scheduler " + ops + " does not schedule RT threads");
    return;
  }
  add(
    rep, "sched-ext", Verdict::Warn,
    "BPF scheduler " + ops + " (" + trimmed(*state) + ") schedules SCHED_OTHER threads: " +
      (rt_policy(opts.policy) ? "this stage's reset children" : "this stage") +
      " run under its policy; its watchdog returns them to the fair class after 30 s stalls");
}

void cgroup_limits(
  Report & rep, const Fs & fs, int tid, const Options & opts, const std::vector<Task> & procs)
{
  const std::string dir = cgroup_dir(fs, tid);
  if (dir.empty()) {
    add(rep, "cgroup-cpu", Verdict::Unknown, "no cgroup v2 line in " + proc_dir(tid) + "/cgroup");
    add(
      rep, "cgroup-memory", Verdict::Unknown, "no cgroup v2 line in " + proc_dir(tid) + "/cgroup");
    return;
  }
  std::vector<std::string> cpu;
  std::vector<std::string> high;
  std::vector<std::string> hard;
  for (const auto & d : cgroup_chain(dir)) {
    const auto max = fs.read(d + "/cpu.max");
    if (max && max->rfind("max", 0) != 0) cpu.push_back(d + " cpu.max " + trimmed(*max));
    const auto h = fs.read(d + "/memory.high");
    if (h && trimmed(*h) != "max") high.push_back(d + " memory.high " + trimmed(*h));
    for (const char * k : {"/memory.max", "/memory.swap.max"}) {
      const auto v = fs.read(d + k);
      if (v && trimmed(*v) != "max") hard.push_back(d + k + " " + trimmed(*v));
    }
  }
  const bool fair_threads = !rt_policy(opts.policy) || opts.children == Children::Reset;
  if (!cpu.empty() && fair_threads) {
    add(
      rep, "cgroup-cpu", Verdict::Warn,
      list_some(cpu, 2) + ": SCHED_OTHER threads here (" +
        (rt_policy(opts.policy) ? "reset children" : "the stage") +
        ") stop until the period ends once the quota is spent");
  } else {
    add(
      rep, "cgroup-cpu", Verdict::Ok,
      cpu.empty() ? "no cpu.max quota" : list_some(cpu, 2) + " limits only SCHED_OTHER");
  }
  const bool oomd = running(procs, "systemd-oomd");
  if (!high.empty() || oomd) {
    add(
      rep, "cgroup-memory", Verdict::Warn,
      (high.empty()
         ? std::string()
         : list_some(high, 2) + ": an allocation past it is throttled in reclaim, RT or not. ") +
        (oomd ? "systemd-oomd runs and kills whole cgroups under memory pressure. " : "") +
        (hard.empty() ? "" : "Also: " + list_some(hard, 3)));
  } else {
    add(rep, "cgroup-memory", Verdict::Ok, hard.empty() ? "no memory limit" : list_some(hard, 3));
  }
}

void memory_kernel(Report & rep, const Fs & fs)
{
  const auto enabled = fs.read("/sys/kernel/mm/transparent_hugepage/enabled");
  if (!enabled) {
    add(rep, "thp", Verdict::Ok, "no transparent huge pages in this kernel");
  } else {
    const std::string e = bracketed(*enabled);
    const std::string d =
      bracketed(fs.read("/sys/kernel/mm/transparent_hugepage/defrag").value_or(""));
    const std::string k =
      trimmed(fs.read("/sys/kernel/mm/transparent_hugepage/khugepaged/defrag").value_or("?"));
    const std::string facts = "enabled " + e + ", defrag " + d + ", khugepaged defrag " + k;
    if (e == "always" || d == "always") {
      add(
        rep, "thp", Verdict::Warn,
        facts +
          ": khugepaged collapses take the mmap_lock for write and flush the TLB, and "
          "defrag=always compacts in the fault path for tens of ms. Use madvise or never, "
          "or PR_SET_THP_DISABLE for the process");
    } else {
      add(rep, "thp", Verdict::Ok, facts);
    }
  }
  const auto unevictable = fs.read("/proc/sys/vm/compact_unevictable_allowed");
  if (!unevictable) {
    add(
      rep, "compaction", Verdict::Unknown, "cannot read /proc/sys/vm/compact_unevictable_allowed");
  } else if (trimmed(*unevictable) == "1") {
    add(
      rep, "compaction", Verdict::Warn,
      "compact_unevictable_allowed=1: compaction migrates mlocked pages, so a locked page can "
      "still fault while it moves; set it to 0");
  } else {
    add(rep, "compaction", Verdict::Ok, "compaction leaves mlocked pages in place");
  }
  const Cpus nodes = parse_cpu_list(fs.read("/sys/devices/system/node/online").value_or("0"));
  const auto balancing = fs.read("/proc/sys/kernel/numa_balancing");
  if (nodes.size() <= 1) {
    add(rep, "numa-balancing", Verdict::Ok, "one NUMA node");
  } else if (balancing && trimmed(*balancing) != "0") {
    add(
      rep, "numa-balancing", Verdict::Warn,
      "numa_balancing=" + trimmed(*balancing) + " on " + std::to_string(nodes.size()) +
        " nodes: task_numa_work unmaps pages to sample faults and migrates them; mlock does not "
        "stop migration. Set it to 0");
  } else {
    add(rep, "numa-balancing", Verdict::Ok, "NUMA balancing off");
  }
  const long long bytes = number(fs.read("/proc/sys/vm/dirty_bytes"), 0);
  const long long background = number(fs.read("/proc/sys/vm/dirty_background_bytes"), 0);
  if (bytes > 0 && background > 0) {
    add(
      rep, "dirty-limits", Verdict::Ok,
      "dirty_bytes " + std::to_string(bytes) + ", dirty_background_bytes " +
        std::to_string(background));
  } else {
    const auto mem = field(fs.read("/proc/meminfo").value_or(""), "MemTotal");
    const long long total_kb = mem ? number(words(*mem).at(0), 0) : 0;
    const long long ratio = number(fs.read("/proc/sys/vm/dirty_ratio"), 20);
    add(
      rep, "dirty-limits", Verdict::Warn,
      "dirty_ratio " + std::to_string(ratio) + "% lets up to about " +
        std::to_string(total_kb * ratio / 100 / 1024 / 1024) +
        " GiB of dirty page cache build up, and a thread writing past it sleeps in "
        "balance_dirty_pages (a ROS log write included). Set vm.dirty_bytes and "
        "vm.dirty_background_bytes");
  }
}

void audit(Report & rep, const Fs & fs, const std::vector<Task> & procs)
{
  const auto param = boot_param(fs.read("/proc/cmdline").value_or(""), "audit");
  if (param && *param == "0") {
    add(rep, "audit", Verdict::Ok, "audit=0 on the boot line");
  } else if (running(procs, "auditd") || (param && *param == "1")) {
    add(
      rep, "audit", Verdict::Warn,
      "audit is on: once enabled, every new task takes the syscall audit path even with no rules, "
      "and a full backlog makes the task that writes a record sleep up to backlog_wait_time. "
      "Add -a never,task, and check auditctl -s");
  } else {
    add(
      rep, "audit", Verdict::Unknown,
      "no audit=1 and no auditd; whether audit is enabled needs auditctl -s as root");
  }
}

void console(Report & rep, const Fs & fs)
{
  const auto consoles = fs.read("/proc/consoles");
  if (!consoles) {
    add(rep, "console", Verdict::Unknown, "cannot read /proc/consoles");
    return;
  }
  std::vector<std::string> serial;
  std::istringstream lines(*consoles);
  std::string line;
  while (std::getline(lines, line)) {
    const auto w = words(line);
    if (
      !w.empty() && w[0].size() > 3 && w[0].rfind("tty", 0) == 0 &&
      std::isalpha(static_cast<unsigned char>(w[0][3])) != 0) {
      serial.push_back(w[0]);
    }
  }
  const std::string level = words(fs.read("/proc/sys/kernel/printk").value_or("?")).at(0);
  if (serial.empty()) {
    add(rep, "console", Verdict::Ok, "no serial console");
  } else if (preempt_rt(fs)) {
    add(
      rep, "console", Verdict::Ok,
      list_some(serial, 3) + ": PREEMPT_RT prints from its own threads");
  } else {
    add(
      rep, "console", Verdict::Warn,
      "serial console " + list_some(serial, 3) + " at console_loglevel " + level +
        ": a kernel message is written out in the context of whoever logged it, at serial speed. "
        "Lower the loglevel or drop the console= entry");
  }
}

void clocksource(Report & rep, const Fs & fs)
{
  const auto cs = fs.read("/sys/devices/system/clocksource/clocksource0/current_clocksource");
  if (!cs) {
    add(rep, "clocksource", Verdict::Unknown, "cannot read current_clocksource");
    return;
  }
  const std::string name = trimmed(*cs);
  if (name == "tsc" || name == "arch_sys_counter" || name == "kvm-clock") {
    add(rep, "clocksource", Verdict::Ok, "clocksource " + name + " reads through the vDSO");
  } else {
    add(
      rep, "clocksource", Verdict::Warn,
      "clocksource " + name +
        ": clock_gettime leaves the vDSO and one read costs about a microsecond");
  }
}

void virtualization(Report & rep, const Fs & fs)
{
  std::istringstream cpuinfo(fs.read("/proc/cpuinfo").value_or(""));
  std::string flags;
  while (std::getline(cpuinfo, flags) && flags.rfind("flags", 0) != 0) {
  }
  const bool guest = (" " + flags + " ").find(" hypervisor ") != std::string::npos ||
                     !trimmed(fs.read("/sys/hypervisor/type").value_or("")).empty() ||
                     fs.exists("/proc/device-tree/hypervisor");
  if (guest) {
    add(
      rep, "virtualization", Verdict::Warn,
      "this is a virtual machine: the host schedules its vcpus, so latency measured here says "
      "nothing about bare metal");
  } else {
    add(rep, "virtualization", Verdict::Ok, "no hypervisor seen");
  }
}

void power_policy(Report & rep, const Fs & fs, const std::vector<Task> & procs)
{
  std::vector<std::string> owners;
  for (const char * d : {"power-profiles-daemon", "tuned", "thermald"}) {
    if (running(procs, d)) owners.push_back(d);
  }
  if (const auto profile = fs.read("/sys/firmware/acpi/platform_profile")) {
    owners.push_back("ACPI platform_profile " + trimmed(*profile));
  }
  if (fs.exists("/etc/nvpmodel.conf")) owners.push_back("Jetson nvpmodel");
  if (owners.empty()) {
    add(rep, "power-policy", Verdict::Ok, "no power policy owner seen");
  } else {
    add(
      rep, "power-policy", Verdict::Warn,
      list_some(owners, 4) +
        " can change governors, EPP, clocks or online cpus after preflight; pin its profile or "
        "stop it");
  }
}

void memory_lock(Report & rep, const Fs & fs, int tid)
{
  const std::string path = proc_dir(tid) + "/status";
  const auto status = fs.read(path);
  if (!status) {
    add(rep, "memory-lock", Verdict::Unknown, "cannot read " + path);
    return;
  }
  const auto vmlck = field(*status, "VmLck");
  if (!vmlck) {
    add(rep, "memory-lock", Verdict::Ok, "a kernel thread: no user memory to fault");
  } else if (number(words(*vmlck).at(0), 0) > 0) {
    add(rep, "memory-lock", Verdict::Ok, "VmLck=" + *vmlck);
  } else {
    add(
      rep, "memory-lock", Verdict::Warn,
      "no locked memory: any page fault is an unbounded stall; lock and pre-fault the working "
      "set (mlockall(MCL_CURRENT | MCL_FUTURE))");
  }
}

bool kernel_thread(const Fs & fs, int tid)
{
  const auto status = fs.read(proc_dir(tid) + "/status");
  return status && !field(*status, "VmSize");
}

}  // namespace

std::string blocking_layers(const Fs & fs)
{
  return layers(fs);
}

void preflight_into(Report & rep, const Fs & fs, int tid, const Options & opts, int control)
{
  const bool rt = rt_policy(opts.policy);
  if (rt) {
    permission(rep, fs, opts);
    rt_throttle(rep, fs, opts);
    priority_order(rep, opts, control);
    priority_band(rep, opts);
  }
  const std::vector<Task> procs = tasks(fs, false);
  if (!opts.cpus.empty()) {
    cpu_online(rep, fs, opts);
    cpu_isolation(rep, fs, opts);
    cpuset(rep, fs, tid, opts);
    cpu_governor(rep, fs, opts);
    cpufreq(rep, fs, opts);
    rcu_offload(rep, fs, opts);
    irq_affinity(rep, fs, opts);
    irq_default(rep, fs, opts);
    irqbalance(rep, procs);
    cpu_idle(rep, fs, opts);
    workqueue(rep, fs, opts);
    kthreads(rep, procs, opts);
    smt(rep, fs, opts);
    capacity(rep, fs, opts);
    softlockup(rep, fs, opts);
  }
  kernel_preemption(rep, fs);
  preempt_mode(rep, fs, opts);
  sched_ext(rep, fs, opts);
  memory_kernel(rep, fs);
  audit(rep, fs, procs);
  console(rep, fs);
  clocksource(rep, fs);
  virtualization(rep, fs);
  power_policy(rep, fs, procs);
  memory_lock(rep, fs, tid);
  if (!kernel_thread(fs, tid)) {
    cgroup_limits(rep, fs, tid, opts, procs);
    secure_exec(rep, fs, tid);
    bind_now(rep, fs, tid);
  }
}

namespace
{

bool rt_stage(const Stage & s)
{
  return s.external ? s.expect_priority > 0 : rt_policy(s.opts.policy);
}

bool comm_is(const std::string & comm, const std::string & full)
{
  return comm == full || (full.size() > 15 && comm == full.substr(0, 15));
}

bool matched(const Stage & s, const Task & t, const std::map<int, std::string> & process)
{
  if (!s.match.irq.empty()) {
    const auto dash = t.comm.find('-');
    return t.comm.rfind("irq/", 0) == 0 && dash != std::string::npos &&
           comm_is(t.comm, t.comm.substr(0, dash + 1) + s.match.irq);
  }
  const auto it = process.find(t.pid);
  return !s.match.thread.empty() && comm_is(t.comm, s.match.thread) && it != process.end() &&
         comm_is(it->second, s.match.process);
}

int native(Policy p)
{
  return p == Policy::Fifo ? 1 : p == Policy::RoundRobin ? 2 : 0;
}

struct Holders
{
  std::set<std::pair<int, int>> threads;
  std::set<int> processes;
};

Holders holders()
{
  Holders h;
  for (const auto & proc : record::read_live()) {
    for (const auto & e : proc.entries) {
      h.threads.insert({e.pid, e.tid});
      h.processes.insert(e.pid);
    }
  }
  return h;
}

bool explained(
  const Spec & spec, const Task & t, const Holders & h, const std::map<int, std::string> & process)
{
  if (h.threads.count({t.pid, t.tid}) != 0) return true;
  return std::any_of(spec.stages().begin(), spec.stages().end(), [&](const Stage & s) {
    if (!s.match.empty()) return matched(s, t, process);
    if (s.external) return s.expect_priority > 0 && s.expect_priority == t.rt_priority;
    return s.opts.children == Children::Inherit && h.processes.count(t.pid) != 0 &&
           native(s.opts.policy) == t.policy && s.opts.priority == t.rt_priority;
  });
}

void foreign_rt(Report & rep, const Spec & spec, const std::vector<Task> & all)
{
  const Holders h = holders();
  std::map<int, std::string> process;
  for (const auto & t : all) {
    if (t.pid == t.tid) process[t.pid] = t.comm;
  }
  bool any = false;
  for (const auto & s : spec.stages()) {
    if (s.external || !rt_stage(s) || s.opts.cpus.empty()) continue;
    std::vector<std::string> above;
    for (const auto & t : all) {
      if ((t.policy != 1 && t.policy != 2) || t.rt_priority < s.opts.priority) continue;
      if (!overlap(t.cpus, s.opts.cpus) || t.comm.rfind("migration/", 0) == 0) continue;
      if (explained(spec, t, h, process)) continue;
      above.push_back(
        t.comm + " (tid " + std::to_string(t.tid) + ", " + (t.policy == 1 ? "FIFO " : "RR ") +
        std::to_string(t.rt_priority) + ")");
    }
    if (above.empty()) continue;
    any = true;
    add(
      rep, "foreign-rt-thread", Verdict::Warn,
      "stage " + detail::stage_name(s) + " (" + std::to_string(s.opts.priority) + " on cpus " +
        join(s.opts.cpus) +
        ") can be preempted by RT threads the file does not declare: " + list_some(above, 4) +
        ". Declare them (external or match), move them off the stage cpus, or lower them");
  }
  if (!any) {
    add(
      rep, "foreign-rt-thread", Verdict::Ok,
      "no undeclared RT thread runs at or above a stage on its cpus");
  }
}

Cpus stage_cpus(const Spec & spec)
{
  Cpus out;
  for (const auto & s : spec.stages()) {
    for (auto c : s.opts.cpus) {
      if (!contains(out, c)) out.push_back(c);
    }
  }
  return out;
}

void deadline(Report & rep, const Spec & spec, const std::vector<Task> & all)
{
  const Cpus used = stage_cpus(spec);
  std::vector<std::string> dl;
  for (const auto & t : all) {
    if (t.policy == kSchedDeadline && overlap(t.cpus, used)) {
      dl.push_back(t.comm + " (cpus " + join(t.cpus) + ")");
    }
  }
  if (dl.empty()) {
    add(rep, "deadline-task", Verdict::Ok, "no SCHED_DEADLINE task on the stage cpus");
  } else {
    add(
      rep, "deadline-task", Verdict::Warn,
      "SCHED_DEADLINE tasks run above every FIFO stage on its cpus: " + list_some(dl, 4) +
        (std::any_of(
           dl.begin(), dl.end(), [](const std::string & d) { return d.rfind("sugov:", 0) == 0; })
           ? ". sugov:N is schedutil's slow path; the performance governor removes it"
           : ""));
  }
}

void housekeeping(Report & rep, const Fs & fs, const Spec & spec)
{
  const Cpus all = online(fs);
  if (all.empty()) {
    add(rep, "housekeeping", Verdict::Unknown, "cannot read /sys/devices/system/cpu/online");
    return;
  }
  Cpus taken = isolated_cpus(fs);
  for (const auto & s : spec.stages()) {
    if (s.strict != Strictness::Hard || !rt_stage(s) || s.kind != Kind::Worker) continue;
    for (auto c : s.opts.cpus) taken.push_back(c);
  }
  Cpus left;
  for (auto c : all) {
    if (!contains(taken, c)) left.push_back(c);
  }
  if (left.empty()) {
    add(
      rep, "housekeeping", Verdict::Fail,
      "isolated cpus and hard stage cpus cover every online cpu: nothing is left for the kernel's "
      "housekeeping, the reset children, or the rest of the system");
  } else {
    add(rep, "housekeeping", Verdict::Ok, "cpus " + join(left) + " stay for housekeeping");
  }
}

void nohz_single(Report & rep, const Fs & fs, const Spec & spec)
{
  const Cpus nohz = parse_cpu_list(fs.read("/sys/devices/system/cpu/nohz_full").value_or(""));
  std::vector<std::string> crowded;
  for (auto c : nohz) {
    std::vector<std::string> on;
    for (const auto & s : spec.stages()) {
      if (contains(s.opts.cpus, c)) on.push_back(detail::stage_name(s));
    }
    if (on.size() > 1) crowded.push_back(cpu_word(c) + ": " + list_some(on, 3));
  }
  if (crowded.empty()) {
    add(rep, "nohz-single-task", Verdict::Ok, "no nohz_full cpu carries two stages");
  } else {
    add(
      rep, "nohz-single-task", Verdict::Warn,
      "the tick stops only on a nohz_full cpu with one runnable task, and these carry two or more "
      "stages: " +
        list_some(crowded, 3));
  }
}

void llc(Report & rep, const Fs & fs, const Spec & spec)
{
  auto domains = [&](const Stage & s) {
    std::set<std::string> out;
    for (auto c : s.opts.cpus) {
      auto d = fs.read(sysfs_cpu(c, "cache/index3/shared_cpu_list"));
      if (!d) d = fs.read(sysfs_cpu(c, "cpufreq/related_cpus"));
      if (d) out.insert(trimmed(*d));
    }
    return out;
  };
  std::vector<std::string> crossings;
  bool known = false;
  for (const auto & chain : detail::chains(spec)) {
    for (std::size_t i = 1; i < chain.size(); ++i) {
      const Stage & a = spec.stages()[chain[i - 1]];
      const Stage & b = spec.stages()[chain[i]];
      if (a.opts.cpus.empty() || b.opts.cpus.empty()) continue;
      const auto da = domains(a);
      const auto db = domains(b);
      if (da.empty() || db.empty()) continue;
      known = true;
      if (std::none_of(
            da.begin(), da.end(), [&](const std::string & d) { return db.count(d) != 0; })) {
        crossings.push_back(detail::stage_name(a) + " -> " + detail::stage_name(b));
      }
    }
  }
  if (!known) {
    add(
      rep, "llc-handoff", Verdict::Unknown,
      "no adjacent pinned stages, or no cache topology in sysfs");
  } else if (crossings.empty()) {
    add(rep, "llc-handoff", Verdict::Ok, "adjacent stages share a last-level cache");
  } else {
    add(
      rep, "llc-handoff", Verdict::Ok,
      "for information: these handoffs cross last-level caches, so each moves its cache lines "
      "between clusters: " +
        list_some(crossings, 3));
  }
}

void smt_neighbor(Report & rep, const Fs & fs, const Spec & spec)
{
  std::vector<std::string> pairs;
  const auto & st = spec.stages();
  for (const auto & a : st) {
    if (a.external || !rt_stage(a) || a.kind != Kind::Worker || a.strict != Strictness::Hard)
      continue;
    for (auto c : a.opts.cpus) {
      for (auto sib :
           parse_cpu_list(fs.read(sysfs_cpu(c, "topology/thread_siblings_list")).value_or(""))) {
        for (const auto & b : st) {
          if (&a == &b || b.external || !rt_stage(b) || b.kind != Kind::Worker || sib == c)
            continue;
          if (contains(b.opts.cpus, sib) && !contains(a.opts.cpus, sib)) {
            pairs.push_back(detail::stage_name(a) + " / " + detail::stage_name(b));
          }
        }
      }
    }
  }
  if (pairs.empty()) {
    add(
      rep, "smt-neighbor", Verdict::Ok,
      "no hard RT worker shares a physical core with another RT worker");
  } else {
    add(
      rep, "smt-neighbor", Verdict::Warn,
      "hard RT workers on SMT siblings of one core: " + list_some(pairs, 3) +
        ". They share the core's pipeline and caches");
  }
}

void taint(Report & rep, const Fs & fs)
{
  const auto text = fs.read("/proc/sys/kernel/tainted");
  if (!text) {
    add(rep, "taint", Verdict::Unknown, "cannot read /proc/sys/kernel/tainted");
    return;
  }
  const long long bits = number(text, 0);
  std::vector<std::string> bad;
  if ((bits >> 9) & 1) bad.emplace_back("W (a kernel warning fired)");
  if ((bits >> 14) & 1) bad.emplace_back("L (a soft lockup was seen)");
  if (bad.empty()) {
    add(rep, "taint", Verdict::Ok, "tainted=" + std::to_string(bits) + ", no W or L");
  } else {
    add(
      rep, "taint", Verdict::Warn,
      "tainted=" + std::to_string(bits) + ": " + list_some(bad, 2) +
        "; read dmesg for the warning or the starved cpu");
  }
}

}  // namespace

Report preflight_spec(const Fs & fs, const Spec & spec)
{
  Report rep;
  const std::vector<Task> all = tasks(fs, true);
  housekeeping(rep, fs, spec);
  foreign_rt(rep, spec, all);
  deadline(rep, spec, all);
  nohz_single(rep, fs, spec);
  smt_neighbor(rep, fs, spec);
  llc(rep, fs, spec);
  taint(rep, fs);
  return rep;
}

}  // namespace cyra::host

namespace cyra
{

Report preflight(const Spec & spec)
{
  return host::preflight_spec(host::Fs{}, spec);
}

Report detail::preflight_at(const std::string & root, const Spec & spec)
{
  return host::preflight_spec(host::Fs(root), spec);
}

}  // namespace cyra
