#ifndef CYRA_RT_HPP
#define CYRA_RT_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace cyra
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

  void validate() const;
};

namespace detail
{
// The syscalls alone, all-or-nothing: a refused part rolls back and throws std::system_error.
// No preflight and no read-back, so the public way in is apply_checked() or bind().
void apply(const Options & opts);

// On thread `tid` (0 = caller). Daemon only: a thread set from outside runs unscheduled until then.
void apply_to(int tid, const Options & opts);
}  // namespace detail

struct ThreadState
{
  int policy = 0;
  int priority = 0;
  std::vector<std::uint32_t> cpus;
  bool reset_on_fork = false;  // SCHED_RESET_ON_FORK, split out of the kernel's policy value
};
ThreadState current();

int this_tid();

// Throws std::system_error when the thread cannot be read, an exited one included: a thread that
// is gone must not read as a thread at the wrong priority.
ThreadState observe(int tid);

// Info is a fact worth knowing that blocks nothing, at any strictness.
enum class Verdict : std::uint8_t { Ok, Warn, Fail, Unknown, Info };

enum class Strictness : std::uint8_t {
  Soft,  // Warn findings are returned, not thrown
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

  bool ok() const;  // no Fail findings
  const Finding * find(const char * id) const;
  std::string to_string() const;
};

// control_priority 0 reports `priority-order` Unknown: a check that did not run must not pass.
// `strict` is how the request will be read: some findings are a Warn only for a hard stage.
Report preflight(
  const Options & opts, int control_priority = 0, Strictness strict = Strictness::Soft);

// A blocking verdict throws before the thread is touched. The result is read back, since an
// accepted request can still land elsewhere; a mismatch throws with the thread already changed.
Report apply_checked(
  const Options & opts, Strictness strict = Strictness::Soft, int control_priority = 0);

namespace detail
{
// The daemon's, on thread `tid`: memory-lock is judged on its process; read-back is observe().
Report preflight_to(
  int tid, const Options & opts, int control_priority, Strictness strict = Strictness::Soft);
Report apply_checked_to(int tid, const Options & opts, Strictness strict, int control_priority);
// preflight_to with every /proc and /sys path read under `root`, for tests on host snapshots.
Report preflight_at(
  const std::string & root, int tid, const Options & opts, int control_priority,
  Strictness strict = Strictness::Soft);
}  // namespace detail

}  // namespace cyra

#endif  // CYRA_RT_HPP
