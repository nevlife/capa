#ifndef CAPA_RT_HPP
#define CAPA_RT_HPP

#include "capa/result.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace capa
{

enum class Policy : std::uint8_t {
  Inherit = 0,  // leave the thread's scheduling class alone
  Other,        // SCHED_OTHER: explicit return to the default class
  Fifo,         // SCHED_FIFO: static priority, runs until it blocks or is preempted
  RoundRobin,   // SCHED_RR: SCHED_FIFO plus a timeslice among equal priorities
};

// Reset is SCHED_RESET_ON_FORK. Affinity is inherited either way: the kernel has no flag for it.
enum class Children : std::uint8_t {
  Reset = 0,  // children start SCHED_OTHER
  Inherit,    // children run as the parent, for fork-join helpers doing the parent's work
};

struct Options
{
  Policy policy = Policy::Inherit;
  int priority = 0;                     // 1..99 with Fifo/RoundRobin, else 0
  std::vector<std::uint32_t> cpus;      // pin to these cores; empty = leave affinity alone
  Children children = Children::Reset;  // takes effect only with Fifo/RoundRobin

  [[nodiscard]] auto validate() const -> Result<void>;
};

namespace detail
{
// The syscalls alone, all-or-nothing: a refused part rolls back and returns a System error.
// No preflight and no read-back, so the public way in is apply_checked() or bind().
auto apply(const Options & opts) -> Result<void>;

// On thread `tid` (0 = caller). Daemon only: a thread set from outside runs unscheduled until then.
auto apply_to(int tid, const Options & opts) -> Result<void>;
}  // namespace detail

struct ThreadState
{
  int policy = 0;
  int priority = 0;
  std::vector<std::uint32_t> cpus;
  bool reset_on_fork = false;  // SCHED_RESET_ON_FORK, split out of the kernel's policy value
};
auto current() -> ThreadState;

auto this_tid() noexcept -> int;  // the kernel tid; -1 off Linux, where capa sets nothing

// A System error when the thread cannot be read, an exited one included: a thread that is gone
// must not read as a thread at the wrong priority.
auto observe(int tid) -> Result<ThreadState>;

// Info is a fact worth knowing that blocks nothing, at any strictness.
enum class Verdict : std::uint8_t { Ok, Warn, Fail, Unknown, Info };

enum class Strictness : std::uint8_t {
  Soft,  // Warn findings are returned, and the apply goes on
  Hard,  // Warn findings block the apply
};

// `id` is a stable slug; tests and tooling key on it.
struct Finding
{
  std::string id;
  Verdict verdict = Verdict::Unknown;
  std::string detail;
};

struct Report
{
  std::vector<Finding> findings;

  [[nodiscard]] auto ok() const -> bool;  // no Fail findings
  auto find(const char * id) const -> const Finding *;
  [[nodiscard]] auto to_string() const -> std::string;
};

// control_priority 0 reports `priority-order` Unknown: a check that did not run must not pass.
// `strict` is how the request will be read: some findings are a Warn only for a hard stage.
auto preflight(const Options & opts, int control_priority = 0, Strictness strict = Strictness::Soft)
  -> Result<Report>;

// A blocking verdict fails before the thread is touched. The result is read back, since an
// accepted request can still land elsewhere; a mismatch fails with the thread already changed.
auto apply_checked(
  const Options & opts, Strictness strict = Strictness::Soft,
  int control_priority = 0) -> Result<Report>;

namespace detail
{
// The daemon's, on thread `tid`: memory-lock is judged on its process; read-back is observe().
auto preflight_to(
  int tid, const Options & opts, int control_priority,
  Strictness strict = Strictness::Soft) -> Result<Report>;
auto apply_checked_to(int tid, const Options & opts, Strictness strict, int control_priority)
  -> Result<Report>;
// Why `got` is not what `opts` asks for, or "" when it is.
auto mismatch(const Options & opts, const ThreadState & got) -> std::string;
// preflight_to with every /proc and /sys path read under `root`, for tests on host snapshots.
auto preflight_at(
  const std::string & root, int tid, const Options & opts, int control_priority,
  Strictness strict = Strictness::Soft) -> Result<Report>;
}  // namespace detail

}  // namespace capa

#endif  // CAPA_RT_HPP
