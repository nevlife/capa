#include "capa/rt.hpp"

#include "host.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#if defined(__linux__)
#include <dirent.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace capa
{

namespace
{

// Matches glibc CPU_SETSIZE; validate() must not depend on <sched.h> being present.
constexpr std::uint32_t kMaxCpus = 1024;

[[noreturn]] void fail(const std::string & why)
{
  throw std::invalid_argument("capa: " + why);
}

auto is_rt_policy(Policy p) -> bool
{
  return p == Policy::Fifo || p == Policy::RoundRobin;
}

auto policy_name(Policy p) -> const char *
{
  switch (p) {
    case Policy::Inherit:
      return "inherit";
    case Policy::Other:
      return "SCHED_OTHER";
    case Policy::Fifo:
      return "SCHED_FIFO";
    case Policy::RoundRobin:
      return "SCHED_RR";
  }
  return "?";
}

#if defined(__linux__)

static_assert(kMaxCpus == CPU_SETSIZE, "cpu id validation must match the kernel set size");

auto refusal(int tid, int err) -> std::string
{
  const std::string layers = err == EPERM ? host::blocking_layers(host::Fs{}) : std::string();
  return (tid != 0 && err == EPERM ? "; setting a thread of another user needs CAP_SYS_NICE"
                                   : "; capa::preflight() explains this host") +
         (layers.empty() ? "" : "; in the way: " + layers);
}

auto native_policy(Policy p) -> int
{
  switch (p) {
    case Policy::Other:
      return SCHED_OTHER;
    case Policy::Fifo:
      return SCHED_FIFO;
    case Policy::RoundRobin:
      return SCHED_RR;
    case Policy::Inherit:
      break;
  }
  return -1;
}

#endif  // defined(__linux__)

}  // namespace

void Options::validate() const
{
  const bool rt = is_rt_policy(policy);
  if (rt && (priority < 1 || priority > 99)) {
    fail(
      std::string(policy_name(policy)) + " needs priority 1..99, got " + std::to_string(priority));
  }
  if (!rt && priority != 0) {
    fail(
      "priority " + std::to_string(priority) + " with policy " + policy_name(policy) +
      " would be ignored; set policy=Fifo or RoundRobin, or drop the priority");
  }
  if (!rt && children == Children::Inherit) {
    fail(
      std::string("children=inherit with policy ") + policy_name(policy) +
      " would be ignored; only an RT policy passes anything on to children");
  }
  for (auto c : cpus) {
    if (c >= kMaxCpus) {
      fail(
        "cpu id " + std::to_string(c) + " exceeds the kernel set size " + std::to_string(kMaxCpus));
    }
  }
}

void detail::apply_to(int tid, const Options & opts)
{
  opts.validate();
  const bool want_policy = opts.policy != Policy::Inherit;
  const bool want_cpus = !opts.cpus.empty();
  if (!want_policy && !want_cpus) {
    return;
  }
#if !defined(__linux__)
  throw std::system_error(
    std::make_error_code(std::errc::function_not_supported), "capa: apply targets Linux");
#else
  cpu_set_t old_set;
  if (want_cpus) {
    if (::sched_getaffinity(tid, sizeof old_set, &old_set) != 0) {
      throw std::system_error(errno, std::generic_category(), "capa: sched_getaffinity");
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    for (auto c : opts.cpus) {
      CPU_SET(static_cast<int>(c), &set);
    }
    if (::sched_setaffinity(tid, sizeof set, &set) != 0) {
      const int e = errno;
      throw std::system_error(
        e, std::generic_category(),
        std::string("capa: sched_setaffinity refused the requested cpus") + refusal(tid, e));
    }
  }
  if (want_policy) {
    sched_param param{};
    param.sched_priority = opts.priority;
    const int reset =
      is_rt_policy(opts.policy) && opts.children == Children::Reset ? SCHED_RESET_ON_FORK : 0;
    const bool flag_set = (::sched_getscheduler(tid) & SCHED_RESET_ON_FORK) != 0;
    if (::sched_setscheduler(tid, native_policy(opts.policy) | reset, &param) != 0) {
      const int e = errno;
      if (want_cpus) {
        ::sched_setaffinity(tid, sizeof old_set, &old_set);
      }
      const std::string why =
        e == EPERM && flag_set && reset == 0
          ? "; this thread was set with children=reset before, and only CAP_SYS_NICE can clear "
            "that"
          : refusal(tid, e);
      throw std::system_error(
        e, std::generic_category(),
        "capa: sched_setscheduler(" + std::string(policy_name(opts.policy)) + ", " +
          std::to_string(opts.priority) + ") refused" + why);
    }
  }
#endif
}

void detail::apply(const Options & opts)
{
  apply_to(0, opts);
}

auto current() -> ThreadState
{
  ThreadState st;
#if defined(__linux__)
  const int raw = ::sched_getscheduler(0);
  st.policy = raw & ~SCHED_RESET_ON_FORK;
  st.reset_on_fork = (raw & SCHED_RESET_ON_FORK) != 0;
  sched_param param{};
  if (::sched_getparam(0, &param) == 0) {
    st.priority = param.sched_priority;
  }
  cpu_set_t set;
  if (::sched_getaffinity(0, sizeof set, &set) == 0) {
    for (int c = 0; c < CPU_SETSIZE; ++c) {
      if (CPU_ISSET(c, &set)) {
        st.cpus.push_back(static_cast<std::uint32_t>(c));
      }
    }
  }
#endif
  return st;
}

auto this_tid() -> int
{
#if !defined(__linux__)
  throw std::system_error(
    std::make_error_code(std::errc::function_not_supported), "capa: this_tid targets Linux");
#else
  return static_cast<int>(::syscall(SYS_gettid));
#endif
}

auto observe(int tid) -> ThreadState
{
  // Not a convenience for self: sched_getscheduler reads the caller when handed 0, so an
  // uninitialized tid would answer about the wrong thread and look like a successful check.
  if (tid <= 0) {
    fail("observe needs a thread id; 0 would read the calling thread instead");
  }
#if !defined(__linux__)
  throw std::system_error(
    std::make_error_code(std::errc::function_not_supported), "capa: observe targets Linux");
#else
  const std::string who = "capa: observe(" + std::to_string(tid) + ")";
  ThreadState st;
  errno = 0;
  const int raw = ::sched_getscheduler(tid);
  if (raw < 0) {
    throw std::system_error(errno, std::generic_category(), who);
  }
  st.policy = raw & ~SCHED_RESET_ON_FORK;
  st.reset_on_fork = (raw & SCHED_RESET_ON_FORK) != 0;
  sched_param param{};
  if (::sched_getparam(tid, &param) != 0) {
    throw std::system_error(errno, std::generic_category(), who + " sched_getparam");
  }
  st.priority = param.sched_priority;
  cpu_set_t set;
  if (::sched_getaffinity(tid, sizeof set, &set) != 0) {
    throw std::system_error(errno, std::generic_category(), who + " sched_getaffinity");
  }
  for (int c = 0; c < CPU_SETSIZE; ++c) {
    if (CPU_ISSET(c, &set)) {
      st.cpus.push_back(static_cast<std::uint32_t>(c));
    }
  }
  return st;
#endif
}

auto Report::ok() const -> bool
{
  for (const auto & f : findings) {
    if (f.verdict == Verdict::Fail) {
      return false;
    }
  }
  return true;
}

auto Report::find(const char * id) const -> const Finding *
{
  for (const auto & f : findings) {
    if (f.id == id) {
      return &f;
    }
  }
  return nullptr;
}

auto Report::to_string() const -> std::string
{
  static const char * words[] = {"ok", "warn", "fail", "unknown", "info"};
  std::string out;
  for (const auto & f : findings) {
    out += f.id;
    out += ": ";
    out += words[static_cast<std::uint8_t>(f.verdict)];
    out += " - ";
    out += f.detail;
    out += '\n';
  }
  return out;
}

auto preflight(const Options & opts, int control_priority, Strictness strict) -> Report
{
  return detail::preflight_to(0, opts, control_priority, strict);
}

auto detail::preflight_to(int tid, const Options & opts, int control_priority, Strictness strict)
  -> Report
{
  return preflight_at("", tid, opts, control_priority, strict);
}

auto detail::preflight_at(
  const std::string & root, int tid, const Options & opts, int control_priority,
  Strictness strict) -> Report
{
  opts.validate();
  Report rep;
  host::preflight_into(rep, host::Fs(root), tid, opts, control_priority, strict);
  return rep;
}

namespace
{
auto has_warn(const Report & rep) -> bool
{
  for (const auto & f : rep.findings) {
    if (f.verdict == Verdict::Warn) {
      return true;
    }
  }
  return false;
}
}  // namespace

auto detail::mismatch(const Options & opts, const ThreadState & got) -> std::string
{
  if (opts.policy != Policy::Inherit) {
    if (got.policy != native_policy(opts.policy) || got.priority != opts.priority) {
      return std::string("capa: apply_checked read back policy ") + std::to_string(got.policy) +
             "/priority " + std::to_string(got.priority) + " after asking for " +
             policy_name(opts.policy) + "/" + std::to_string(opts.priority) +
             "; the thread is not running as declared";
    }
    const bool want_reset = is_rt_policy(opts.policy) && opts.children == Children::Reset;
    if (got.reset_on_fork != want_reset) {
      return std::string("capa: apply_checked read back SCHED_RESET_ON_FORK ") +
             (got.reset_on_fork ? "set" : "clear") + " after asking for children=" +
             (opts.children == Children::Reset ? "reset" : "inherit");
    }
  }
  if (!opts.cpus.empty()) {
    for (auto c : got.cpus) {
      if (std::find(opts.cpus.begin(), opts.cpus.end(), c) == opts.cpus.end()) {
        return "capa: apply_checked read back cpu " + std::to_string(c) +
               ", which was not among the requested cpus; the affinity did not take";
      }
    }
    if (got.cpus.empty()) {
      return "capa: apply_checked read back an empty cpu affinity";
    }
  }
  return {};
}

auto detail::apply_checked_to(
  int tid, const Options & opts, Strictness strict, int control_priority) -> Report
{
  opts.validate();
  // Same no-op contract as apply(): options that ask for nothing get nothing, including no
  // judgement. Preflighting here would report a host that no caller asked anything of.
  if (opts.policy == Policy::Inherit && opts.cpus.empty()) {
    return Report{};
  }

  Report rep = detail::preflight_to(tid, opts, control_priority, strict);
  const bool blocked = !rep.ok() || (strict == Strictness::Hard && has_warn(rep));
  if (blocked) {
    throw std::runtime_error(
      std::string("capa: preflight refused this ") +
      (strict == Strictness::Hard ? "hard" : "soft") + "-RT request; nothing was applied\n" +
      rep.to_string());
  }
  detail::apply_to(tid, opts);

  // Confirm by reading back rather than by trusting the syscall's return: nothing stops another
  // thread from re-scheduling this one between the two calls. Trusting the return is the same
  // mistake as trusting apply() without preflight, one layer down.
  //
  // The affinity claim is observed within requested, not equal to it. A cpuset narrows the mask
  // to the cpus it allows, and a thread on fewer of its requested cpus has left none of them;
  // a cpu outside the request is the failure. verify() reads the same claim afterwards.
#if defined(__linux__)
  const std::string off = detail::mismatch(opts, tid == 0 ? current() : observe(tid));
  if (!off.empty()) {
    throw std::runtime_error(off);
  }
#endif
  return rep;
}

auto apply_checked(const Options & opts, Strictness strict, int control_priority) -> Report
{
  return detail::apply_checked_to(0, opts, strict, control_priority);
}

}  // namespace capa
