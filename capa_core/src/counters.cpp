#include "host.hpp"
#include "parse_number.hpp"

#include <sched.h>

#include <cctype>
#include <map>
#include <sstream>
#include <thread>

namespace capa::host
{

namespace
{

void add(Report & rep, const char * id, Verdict v, std::string detail)
{
  rep.findings.push_back(Finding{id, v, std::move(detail)});
}

std::string task_dir(int pid, int tid)
{
  return "/proc/" + std::to_string(pid) + "/task/" + std::to_string(tid);
}

int pid_of(const Fs & fs, int tid)
{
  const auto status = fs.read("/proc/" + std::to_string(tid) + "/status");
  const auto tgid = status ? field(*status, "Tgid") : std::nullopt;
  return tgid ? detail::parse_number<int>(*tgid).value_or(tid) : tid;
}

std::optional<std::uint64_t> sched_value(const std::string & text, const std::string & key)
{
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    if (line.rfind(key, 0) != 0 || line.find(':') == std::string::npos) continue;
    if (line.find_first_not_of(' ', key.size()) != line.find(':')) continue;
    return detail::parse_number<std::uint64_t>(trimmed(line.substr(line.find(':') + 1)));
  }
  return std::nullopt;
}

std::uint64_t flat_value(const std::string & text, const std::string & key)
{
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    const auto w = words(line);
    if (w.size() == 2 && w[0] == key) return detail::parse_number<std::uint64_t>(w[1]).value_or(0);
  }
  return 0;
}

bool bit(const std::optional<std::string> & hex, int signal)
{
  return hex && ((std::stoull(*hex, nullptr, 16) >> (signal - 1)) & 1) != 0;
}

struct Row
{
  std::string desc;
  std::map<std::uint32_t, std::uint64_t> per_cpu;
};

// /proc/interrupts and /proc/softirqs: a CPUn header, then one row per source.
std::map<std::string, Row> per_cpu_table(const Fs & fs, const std::string & path)
{
  std::map<std::string, Row> out;
  std::istringstream lines(fs.read(path).value_or(""));
  std::string line;
  std::vector<std::uint32_t> cols;
  if (std::getline(lines, line)) {
    for (const auto & w : words(line)) {
      if (w.rfind("CPU", 0) == 0)
        cols.push_back(detail::parse_number<std::uint32_t>(w.substr(3)).value_or(0));
    }
  }
  while (std::getline(lines, line)) {
    const auto colon = line.find(':');
    if (colon == std::string::npos) continue;
    const auto w = words(line.substr(colon + 1));
    if (w.size() < cols.size()) continue;
    Row r;
    for (std::size_t i = 0; i < cols.size(); ++i) {
      r.per_cpu[cols[i]] = detail::parse_number<std::uint64_t>(w[i]).value_or(0);
    }
    for (std::size_t i = cols.size(); i < w.size(); ++i)
      r.desc += (r.desc.empty() ? "" : " ") + w[i];
    out[trimmed(line.substr(0, colon))] = std::move(r);
  }
  return out;
}

struct Snapshot
{
  std::optional<Task> task;
  std::optional<std::string> sched;
  std::map<std::string, Row> interrupts;
  std::map<std::string, Row> softirqs;
  std::map<std::string, std::uint64_t> cgroup;
};

Snapshot snap(const Fs & fs, int pid, int tid, const std::vector<std::string> & cgroups)
{
  Snapshot s;
  s.task = task(fs, pid, tid);
  s.sched = fs.read(task_dir(pid, tid) + "/sched");
  s.interrupts = per_cpu_table(fs, "/proc/interrupts");
  s.softirqs = per_cpu_table(fs, "/proc/softirqs");
  for (const auto & d : cgroups) {
    const std::string cpu = fs.read(d + "/cpu.stat").value_or("");
    const std::string mem = fs.read(d + "/memory.events").value_or("");
    for (const char * k : {"nr_throttled", "throttled_usec"})
      s.cgroup[d + " " + k] = flat_value(cpu, k);
    s.cgroup[d + " memory.events high"] = flat_value(mem, "high");
  }
  return s;
}

std::uint64_t delta(const Row & before, const Row & after, std::uint32_t cpu)
{
  const auto b = before.per_cpu.find(cpu);
  const auto a = after.per_cpu.find(cpu);
  if (a == after.per_cpu.end() || b == before.per_cpu.end()) return 0;
  return a->second - b->second;
}

std::string interrupt_remedy(const std::string & name, const std::string & desc)
{
  if (std::isdigit(static_cast<unsigned char>(name[0])) != 0) {
    return "steer it away with /proc/irq/" + name + "/smp_affinity_list";
  }
  if (name == "RES" || desc.find("Rescheduling") != std::string::npos) {
    return "another task wakes or is balanced onto this cpu: isolcpus, or move it";
  }
  if (name == "CAL" || desc.find("Function call") != std::string::npos) {
    return "another cpu runs a function here: keep that work off the stage cpus";
  }
  if (name == "TLB" || desc.find("TLB") != std::string::npos) {
    return "a thread of this process maps or unmaps memory: keep mappings fixed while running";
  }
  if (name == "IWI" || desc.find("IRQ work") != std::string::npos) {
    return "irq_work (perf, printk, RCU): find the sender with trace-cmd -e ipi";
  }
  if (desc.find("broadcast") != std::string::npos) {
    return "an idle state stops the local timer: cap C-states";
  }
  return "find the sender with trace-cmd -e irq -e ipi";
}

const std::map<std::string, std::string> & softirq_remedies()
{
  static const std::map<std::string, std::string> r = {
    {"HI", "device IRQ affinity"},
    {"TIMER", "nohz_full"},
    {"NET_TX", "NIC IRQ affinity"},
    {"NET_RX", "NIC IRQ affinity, or a threaded NAPI stage"},
    {"BLOCK", "block device IRQ affinity"},
    {"IRQ_POLL", "device IRQ affinity"},
    {"TASKLET", "device IRQ affinity"},
    {"SCHED", "isolcpus=domain"},
    {"HRTIMER", "nohz_full, and no timers armed from this cpu"},
    {"RCU", "rcu_nocbs"},
  };
  return r;
}

bool tick(const std::string & name, const std::string & desc)
{
  return name == "LOC" || desc.find("arch_timer") != std::string::npos;
}

void judge_interrupts(
  Report & rep, const Fs & fs, const Snapshot & a, const Snapshot & b, const Cpus & cpus,
  double seconds)
{
  if (a.interrupts.empty()) {
    add(rep, "cpu-interrupts", Verdict::Unknown, "cannot read /proc/interrupts");
    return;
  }
  const Cpus nohz = parse_cpu_list(fs.read("/sys/devices/system/cpu/nohz_full").value_or(""));
  std::vector<std::string> noise;
  std::vector<std::string> ticks;
  for (const auto & [name, before] : a.interrupts) {
    const auto it = b.interrupts.find(name);
    if (it == b.interrupts.end()) continue;
    for (auto c : cpus) {
      const std::uint64_t d = delta(before, it->second, c);
      if (d == 0) continue;
      const std::string what = "cpu" + std::to_string(c) + " " + name + " " + std::to_string(d);
      if (tick(name, before.desc)) {
        const double rate = static_cast<double>(d) / seconds;
        if (contains(nohz, c) && rate > 10) {
          noise.push_back(
            what +
            " (the tick still runs on a nohz_full cpu: more than one task, or a timer, keeps it)");
        } else {
          ticks.push_back(what);
        }
        continue;
      }
      noise.push_back(what + " (" + before.desc + "; " + interrupt_remedy(name, before.desc) + ")");
    }
  }
  if (noise.empty()) {
    add(
      rep, "cpu-interrupts", Verdict::Ok,
      ticks.empty() ? "no interrupt on the stage cpus" : "only the tick: " + list_some(ticks, 4));
  } else {
    add(
      rep, "cpu-interrupts", Verdict::Warn, "interrupts on the stage cpus: " + list_some(noise, 5));
  }
}

void judge_softirqs(Report & rep, const Snapshot & a, const Snapshot & b, const Cpus & cpus)
{
  if (a.softirqs.empty()) {
    add(rep, "cpu-softirqs", Verdict::Unknown, "cannot read /proc/softirqs");
    return;
  }
  std::vector<std::string> fired;
  for (const auto & [name, before] : a.softirqs) {
    const auto it = b.softirqs.find(name);
    if (it == b.softirqs.end()) continue;
    for (auto c : cpus) {
      const std::uint64_t d = delta(before, it->second, c);
      if (d == 0) continue;
      const auto r = softirq_remedies().find(name);
      fired.push_back(
        "cpu" + std::to_string(c) + " " + name + " " + std::to_string(d) +
        (r == softirq_remedies().end() ? "" : " (" + r->second + ")"));
    }
  }
  if (fired.empty()) {
    add(rep, "cpu-softirqs", Verdict::Ok, "no softirq on the stage cpus");
  } else {
    add(rep, "cpu-softirqs", Verdict::Warn, "softirqs on the stage cpus: " + list_some(fired, 6));
  }
}

void judge_thread(Report & rep, const Stage & stage, const Snapshot & a, const Snapshot & b)
{
  const bool rt = stage.opts.policy == Policy::Fifo || stage.opts.policy == Policy::RoundRobin;
  if (!a.task || !b.task) {
    add(rep, "thread-faults", Verdict::Unknown, "cannot read the thread's stat");
  } else {
    const auto minor = b.task->minflt - a.task->minflt;
    const auto major = b.task->majflt - a.task->majflt;
    const std::string what =
      std::to_string(minor) + " minor and " + std::to_string(major) + " major fault(s)";
    add(
      rep, "thread-faults", rt && minor + major > 0 ? Verdict::Warn : Verdict::Ok,
      what + (rt && minor + major > 0 ? ": the working set is not locked and pre-faulted" : ""));
  }
  const auto m0 = a.sched ? sched_value(*a.sched, "se.nr_migrations") : std::nullopt;
  const auto m1 = b.sched ? sched_value(*b.sched, "se.nr_migrations") : std::nullopt;
  const auto p0 = a.sched ? sched_value(*a.sched, "nr_involuntary_switches") : std::nullopt;
  const auto p1 = b.sched ? sched_value(*b.sched, "nr_involuntary_switches") : std::nullopt;
  if (!m0 || !m1 || !p0 || !p1) {
    add(rep, "thread-migrations", Verdict::Unknown, "cannot read the thread's sched file");
    add(rep, "thread-preemptions", Verdict::Unknown, "cannot read the thread's sched file");
    return;
  }
  const bool pinned = stage.opts.cpus.size() == 1;
  const std::uint64_t moved = m1.value_or(0) - m0.value_or(0);
  const std::uint64_t preempted = p1.value_or(0) - p0.value_or(0);
  add(
    rep, "thread-migrations", pinned && moved > 0 ? Verdict::Warn : Verdict::Ok,
    std::to_string(moved) + " migration(s)" +
      (pinned && moved > 0 ? ": a thread pinned to one cpu moved, so its affinity changed" : ""));
  add(
    rep, "thread-preemptions", rt && preempted > 0 ? Verdict::Warn : Verdict::Ok,
    std::to_string(preempted) + " involuntary switch(es)" +
      (rt && preempted > 0 ? ": something above it took its cpu; only a declared release stage "
                             "on that cpu should"
                           : ""));
}

void judge_cgroup(Report & rep, const Snapshot & a, const Snapshot & b)
{
  if (a.cgroup.empty()) {
    add(rep, "cgroup-throttle", Verdict::Unknown, "no cgroup v2 directory for the thread");
    return;
  }
  std::vector<std::string> grew;
  for (const auto & [k, v] : a.cgroup) {
    const auto it = b.cgroup.find(k);
    if (it != b.cgroup.end() && it->second > v)
      grew.push_back(k + " +" + std::to_string(it->second - v));
  }
  if (grew.empty()) {
    add(rep, "cgroup-throttle", Verdict::Ok, "no cgroup throttling");
  } else {
    add(
      rep, "cgroup-throttle", Verdict::Warn,
      "the thread's cgroup throttled it: " + list_some(grew, 4));
  }
}

// A process-directed signal goes to the thread the kernel suggests if that thread wants it, and
// only otherwise to another: complete_signal() and wants_signal() in kernel/signal.c. kill() and
// a terminal suggest the first thread (pid == tid). SIGCHLD suggests the child's parent, which is
// the thread that forked it (copy_process sets real_parent = current).
void judge_signals(Report & rep, const Fs & fs, int pid, int tid, bool rt)
{
  const auto status = fs.read(task_dir(pid, tid) + "/status");
  if (!rt) {
    add(rep, "signals", Verdict::Unknown, "not an RT thread, so its signals are not judged");
    return;
  }
  if (!status) {
    add(rep, "signals", Verdict::Unknown, "cannot read " + task_dir(pid, tid) + "/status");
    return;
  }
  const auto leader = tid == pid ? status : fs.read(task_dir(pid, pid) + "/status");
  if (!leader) {
    add(rep, "signals", Verdict::Unknown, "cannot read " + task_dir(pid, pid) + "/status");
    return;
  }
  const auto blocked = field(*status, "SigBlk");
  const auto caught = field(*status, "SigCgt");
  const auto leader_blocked = field(*leader, "SigBlk");
  const auto state = field(*leader, "State").value_or("");
  const bool leader_exited = !state.empty() && (state[0] == 'Z' || state[0] == 'X');
  const std::string first = "the first thread (tid " + std::to_string(pid) + ")";
  std::vector<std::string> warn;
  std::vector<std::string> info;
  for (const auto & [sig, name] : {std::pair{2, "SIGINT"}, {15, "SIGTERM"}}) {
    if (!bit(caught, sig) || bit(blocked, sig)) continue;
    if (tid == pid) {
      warn.push_back(std::string(name) + " (this is the first thread, which takes it first)");
    } else if (leader_exited) {
      warn.push_back(std::string(name) + " (" + first + " has exited, so any thread takes it)");
    } else if (bit(leader_blocked, sig)) {
      warn.push_back(std::string(name) + " (" + first + " blocks it, so another thread takes it)");
    } else {
      info.emplace_back(name);
    }
  }
  if (bit(caught, 17) && !bit(blocked, 17)) {
    warn.emplace_back(
      "SIGCHLD (it goes to the thread that forked the child, and to any thread while that one "
      "blocks it, as system() does)");
  }
  if (!warn.empty()) {
    add(
      rep, "signals", Verdict::Warn,
      list_some(warn, 3) +
        ": caught by the process and not blocked here, so the handler may run on this RT thread; "
        "block them in RT threads and take them on one SCHED_OTHER thread");
  } else if (!info.empty()) {
    add(
      rep, "signals", Verdict::Info,
      list_some(info, 2) + " are caught by the process and not blocked here, but " + first +
        " takes a process signal first. This thread takes one only while that thread blocks it, "
        "is exiting or stopped, or sleeps with a signal already pending");
  } else {
    add(rep, "signals", Verdict::Ok, "no process signal handler can run on it");
  }
}

}  // namespace

void verify_now(Report & rep, const Fs & fs, int tid, const ThreadState & st)
{
  const int pid = pid_of(fs, tid);
  const bool rt = st.policy == SCHED_FIFO || st.policy == SCHED_RR;
  const auto t = task(fs, pid, tid);
  if (!t) {
    add(rep, "pi-boosted", Verdict::Unknown, "cannot read " + task_dir(pid, tid) + "/stat");
  } else {
    const int expected = rt ? -1 - st.priority : st.policy == SCHED_OTHER ? 0 : t->prio;
    if (t->prio < expected) {
      add(
        rep, "pi-boosted", Verdict::Warn,
        "runs boosted (stat priority " + std::to_string(t->prio) + ", static " +
          std::to_string(st.priority) +
          "): a higher thread waits on a PI lock it holds. Drift is judged on the static value");
    } else {
      add(rep, "pi-boosted", Verdict::Ok, "not boosted");
    }
  }
  judge_signals(rep, fs, pid, tid, rt);
  const auto slack = fs.read("/proc/" + std::to_string(tid) + "/timerslack_ns");
  if (!slack) {
    add(
      rep, "timerslack", Verdict::Unknown,
      "cannot read /proc/" + std::to_string(tid) + "/timerslack_ns");
  } else {
    add(
      rep, "timerslack", Verdict::Ok,
      "timerslack_ns " + trimmed(*slack) +
        (rt ? " (RT threads sleep without slack)"
            : "; a SCHED_OTHER timeout ends up to this late"));
  }
}

void verify_window(Report & rep, const Stage & stage, int tid, std::chrono::milliseconds window)
{
  const Fs fs;
  const int pid = pid_of(fs, tid);
  const auto cgroups = cgroup_chain(cgroup_dir(fs, tid));
  const Snapshot a = snap(fs, pid, tid, cgroups);
  std::this_thread::sleep_for(window);
  const Snapshot b = snap(fs, pid, tid, cgroups);
  const Cpus cpus = !stage.opts.cpus.empty() ? stage.opts.cpus : b.task ? b.task->cpus : Cpus{};
  const double seconds = std::chrono::duration<double>(window).count();
  judge_thread(rep, stage, a, b);
  judge_interrupts(rep, fs, a, b, cpus, seconds);
  judge_softirqs(rep, a, b, cpus);
  judge_cgroup(rep, a, b);
}

}  // namespace capa::host
