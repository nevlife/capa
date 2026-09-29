#ifndef CYRA_RT_HPP
#define CYRA_RT_HPP

#include <cstdint>
#include <string>
#include <vector>

// Thread scheduling for the hard-RT path. Everything here is opt-in; the default-constructed
// Options is a full no-op.

namespace cyra
{

enum class Policy : std::uint8_t {
  Inherit = 0,  // leave the thread's scheduling class alone
  Other,        // SCHED_OTHER: explicit return to the default class
  Fifo,         // SCHED_FIFO: static priority, runs until it blocks or is preempted
  RoundRobin,   // SCHED_RR: SCHED_FIFO plus a timeslice among equal priorities
};

// What threads created by the scheduled thread start with. Linux hands a child its parent's
// policy, priority and affinity, so an RT thread that calls into a library leaks RT into every
// pool, logger and system() that library starts. Reset stops the leak for policy and priority
// (SCHED_RESET_ON_FORK, which covers pthread_create as well as fork). Affinity is inherited
// either way; the kernel has no flag for it. Once a thread is set with Reset, only CAP_SYS_NICE
// can take the flag off it again, so the same thread cannot later switch to Inherit without it.
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

  // Rejects instead of reinterpreting.
  void validate() const;
};

namespace detail
{
// The syscalls alone, on the calling thread, all-or-nothing: if the kernel refuses any part, what
// was already changed is rolled back and std::system_error is thrown. No preflight and no
// read-back, which is why it is not the public way in: apply_checked() and bind() are.
void apply(const Options & opts);

// The same on thread `tid` (0 = the calling thread). Setting a thread from outside is how the
// daemon handles threads it cannot run code on, and nothing else does it: the thread runs
// unscheduled until the daemon finds it, which a self-applying thread never does.
void apply_to(int tid, const Options & opts);
}  // namespace detail

// What the calling thread actually has, read back from the kernel. `policy` is the raw
// SCHED_* value: a thread may hold a policy this header does not name.
struct ThreadState
{
  int policy = 0;
  int priority = 0;
  std::vector<std::uint32_t> cpus;
  bool reset_on_fork = false;  // SCHED_RESET_ON_FORK, split out of the kernel's policy value
};
ThreadState current();

// The calling thread's kernel thread id. std::thread::id names nothing the kernel knows, so a
// thread that expects to be checked publishes this and observe() takes it.
int this_tid();

// What another thread runs at, read from the kernel exactly as current() reads self. Setting is
// refused across threads but reading is not, and the two are not the same risk:
// an observer opens no window where the thread is half-configured and leaves no silent failure if
// it never runs. That asymmetry is what lets a chain be checked whole, the stages cyra does not
// set included. Works across processes: an external stage is another node's thread.
//
// Throws std::system_error when the thread cannot be read, a thread that has exited included: a
// thread that is gone and a thread at the wrong priority must not arrive as the same answer.
ThreadState observe(int tid);

enum class Verdict : std::uint8_t { Ok, Warn, Fail, Unknown };

// How much of preflight's judgement blocks an apply. Fail always blocks under
// both: the request cannot take effect as asked. Warn is the split. The request does take
// effect, but the host is not configured for bounded latency, which soft RT tolerates and hard
// RT does not. A caller that never states which it is gets the soft reading, so declaring Hard
// is what turns those findings from a report nobody reads into a refusal.
enum class Strictness : std::uint8_t {
  Soft,  // Warn findings are returned, not thrown
  Hard,  // Warn findings block the apply
};

// One preflight check. `id` is a stable slug; tests and tooling key on it.
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

// Judge whether `opts` can take effect on this host, and why not, before touching
// anything. Every verdict comes from a kernel-readable source (rlimits, /proc, /sys),
// not from guessing. `control_priority` declares the consumer's control
// loop RT priority (0 = not declared); transport work must stay strictly below it. Left
// undeclared, `priority-order` is reported Unknown rather than omitted. A check that did
// not run and a check that passed must not look alike.
Report preflight(const Options & opts, int control_priority = 0);

// preflight() then the syscalls, which is what the two are for: the syscalls succeed whenever the
// kernel accepts the request, and the kernel accepts plenty of hosts that cannot hold a deadline.
// Throws std::runtime_error with the report when the verdicts block, before
// touching the thread; otherwise applies and returns the report so a Soft caller can log what it
// tolerated. Nothing is applied on a throw. Options that ask for nothing are a full no-op here
// too, judgement included. There is no host to judge against an empty request.
//
// The applied state is confirmed by reading it back, not by trusting the syscall's return: a
// request the kernel accepted can still land elsewhere. A mismatch throws, having already changed
// the thread. The caller is told what it actually got rather than left believing the request.
Report apply_checked(
  const Options & opts, Strictness strict = Strictness::Soft, int control_priority = 0);

namespace detail
{
// preflight() for thread `tid` (0 = the calling thread): memory-lock is judged on that thread's
// process, not the caller's. The daemon's, which sets threads of other processes.
Report preflight_to(int tid, const Options & opts, int control_priority);
// apply_checked() on thread `tid` (0 = the calling thread), judged by preflight_to() and read back
// with observe(). The daemon's.
Report apply_checked_to(int tid, const Options & opts, Strictness strict, int control_priority);
}  // namespace detail

}  // namespace cyra

#endif  // CYRA_RT_HPP
