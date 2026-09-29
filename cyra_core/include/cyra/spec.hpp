#ifndef CYRA_SPEC_HPP
#define CYRA_SPEC_HPP

#include "cyra/rt.hpp"

#include <exception>
#include <future>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// The chain declaration file: one place that says how every thread on a
// dataflow path must be scheduled, read by each process as it starts.
//
// Here rather than in a language wrapper so C++ and Python read the file through one parser. Two
// parsers would be two places for the ordering and collision rules to drift apart.

namespace cyra
{

// A worker computes. A release thread only hands work on (an IRQ thread, a DDS receive thread, a
// timer thread) and must run above the workers it feeds on the same cpu, or the work is released
// late. The two orderings point opposite ways, which is why they are different kinds.
enum class Kind : std::uint8_t { Worker = 0, Release };

// How the daemon finds a thread no code of ours runs on: an interrupt thread by the name its
// handler registered, or a thread by its process's and its own comm. Empty for other stages.
struct Match
{
  std::string irq;      // /sys/kernel/irq/<n>/actions, e.g. "3550000.usb"
  std::string process;  // /proc/<pid>/comm of the owning process
  std::string thread;   // /proc/<pid>/task/<tid>/comm

  bool empty() const noexcept { return irq.empty() && process.empty() && thread.empty(); }
};

// One thread on the chain.
//
// Named `node` + `group` for a ROS thread, or `name` for anything else. `external` marks a thread
// cyra cannot set: an rmw listener, a CUDA worker, a node handed to some other executor. Declared
// anyway, so a stage cyra merely omits and a stage cyra does not own do not look alike.
struct Stage
{
  std::string node;   // fully-qualified ROS node name; empty for a `name` stage
  std::string group;  // group label; empty means the node's default callback group
  std::string name;   // a non-ROS thread; empty for a `node` stage
  Kind kind = Kind::Worker;
  bool external = false;
  int expect_priority = 0;  // external only: what the thread is expected to already run at

  Match match;  // set: the daemon finds and sets this thread, and bind() refuses it
  std::vector<std::uint32_t> children_cpus;  // the daemon moves reset children here

  Options opts;                          // what cyra applies (empty for external)
  Strictness strict = Strictness::Soft;  // the strictest `target` among the chains it sits on

  std::string chain;  // which chain declared this, for error messages
};

namespace detail
{
// How messages spell a stage: `node`, `node/group`, or `name`.
std::string stage_name(const Stage & stage);
// An external stage is a record of a thread cyra does not set. `caller` names the refusing call.
void reject_external(const Stage & stage, const char * caller);
}  // namespace detail

// A parsed declaration file. Every rule it enforces is checked once, here, against the whole
// file. That is why the declarations live in one file: per-node settings cannot check an
// ordering no single node can see.
class Spec
{
public:
  Spec() = default;

  // Parse `path`. Throws std::runtime_error on any violation (docs/en/api.en.md, cyra::Spec).
  static Spec load(const std::string & path);

  // Parse the path in CYRA_SPEC. Unset leaves an empty spec, which is the no-op every node
  // that declares nothing already gets.
  static Spec load();

  bool empty() const noexcept { return stages_.empty(); }
  const std::vector<Stage> & stages() const noexcept { return stages_; }

  // The stage declared for `target` and `group`. `target` is a fully-qualified node name (leading
  // '/') with the group label, or a non-ROS stage's name with no group. Throws if the file does
  // not name it: applying nothing would hide a typo.
  const Stage & stage(const std::string & target, const std::string & group = "") const;

  // Same lookup without the throw; nullptr when absent.
  const Stage * find(const std::string & target, const std::string & group = "") const noexcept;

  // Stages cyra does not set. A caller that can resolve them to threads (a launch tool, an
  // operator) checks them; cyra only records what was declared.
  std::vector<const Stage *> external() const;

private:
  std::vector<Stage> stages_;
};

// Put a declared stage on the calling thread, whether an executor's group thread or a worker (an
// inference loop, a driver poll) no executor owns. Takes the stage whole: unpacking it into
// Options drops the chain's strictness. The ordering against the rest of the chain was checked
// when the file was loaded, so the report carries `priority-order` as Ok rather than asking for a
// control priority. Rejects an external stage, and a stage another live thread of this process
// already holds.
Report apply_checked(const Stage & stage);

// The calling thread runs as the stage CYRA_SPEC declares for `target` and `group` (see
// Spec::stage). Spec::load() + stage() + apply_checked() in one call. CYRA_SPEC unset is a no-op
// and an empty report.
Report bind(const std::string & target, const std::string & group = "");

// A new thread that runs `fn` as the stage for `target` and `group`. Returns once the thread is
// set; if bind() refuses, the thread ends without running `fn` and the refusal is rethrown here.
// The order is the point: a thread started first and bound inside runs unscheduled until it gets
// to the bind() line.
template <class F>
std::thread spawn(const std::string & target, const std::string & group, F && fn)
{
  std::promise<void> ready;
  auto bound = ready.get_future();
  std::thread t([&ready, target, group, fn = std::forward<F>(fn)]() mutable {
    try {
      bind(target, group);
    } catch (...) {
      ready.set_exception(std::current_exception());
      return;
    }
    ready.set_value();
    fn();
  });
  try {
    bound.get();
  } catch (...) {
    t.join();
    throw;
  }
  return t;
}

// Check a thread against what the file declared for it, without setting anything: the only
// thing cyra does for an `external` stage, and for the stages it sets the check that outlives
// the apply (`chrt`, systemd or a later apply can move a thread afterwards).
//
// `tid` is the thread to read (this_tid on the thread itself, or a tid the caller resolved).
// The file names stages by node and label, and nothing in it maps a label to a kernel thread.
Report verify(const Stage & stage, int tid);

// Every RT thread of this process the file cannot explain. A thread is explained when it holds a
// stage (apply_checked or bind), or when it runs at the declared scheduling of a stage whose
// children inherit: a fork-join child of such a stage needs no declaration of its own.
Report verify_process(const Spec & spec);

// The same question about another process, whose stage holders are read from its record.
Report verify_process(const Spec & spec, int pid);

// A stage held by a live thread on this host, as the holding process recorded it.
struct Binding
{
  std::string node;
  std::string group;
  std::string name;
  int pid = 0;
  int tid = 0;
};

// Every stage held on this host. Read from each process's record; a record whose process has
// exited, and an entry whose thread has ended, are left out. Reading changes nothing.
std::vector<Binding> bindings();

namespace detail
{
// Publish this process's record as holding `held`, threads of other processes included. The
// daemon's: a process that binds keeps its record through apply_checked(stage).
std::string record_bindings(const std::vector<Binding> & held);
// /proc/<pid>/task/<tid>/stat field 22, 0 when unreadable. Tells a thread from its tid's reuse.
std::uint64_t task_starttime(int pid, int tid);
}  // namespace detail

}  // namespace cyra

#endif  // CYRA_SPEC_HPP
