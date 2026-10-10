#ifndef CYRA_SPEC_HPP
#define CYRA_SPEC_HPP

#include "cyra/rt.hpp"

#include <chrono>
#include <cstddef>
#include <exception>
#include <future>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// The one parser for C++ and Python, so the file rules cannot drift between languages.

namespace cyra
{

enum class Kind : std::uint8_t { Worker = 0, Release };

struct Match
{
  std::string irq;      // /sys/kernel/irq/<n>/actions, e.g. "3550000.usb"
  std::string process;  // /proc/<pid>/comm of the owning process
  std::string thread;   // /proc/<pid>/task/<tid>/comm

  bool empty() const noexcept { return irq.empty() && process.empty() && thread.empty(); }
};

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

class Spec;

namespace detail
{
// How messages spell a stage: `node`, `node/group`, or `name`.
std::string stage_name(const Stage & stage);
void reject_external(const Stage & stage, const char * caller);
// Each chain's stages, as indices into stages(), in flow order.
const std::vector<std::vector<std::size_t>> & chains(const Spec & spec);
}  // namespace detail

class Spec
{
public:
  Spec() = default;

  static Spec load(const std::string & path);

  static Spec load();

  bool empty() const noexcept { return stages_.empty(); }
  const std::vector<Stage> & stages() const noexcept { return stages_; }

  const Stage & stage(const std::string & target, const std::string & group = "") const;

  const Stage * find(const std::string & target, const std::string & group = "") const noexcept;

  std::vector<const Stage *> external() const;

private:
  friend const std::vector<std::vector<std::size_t>> & detail::chains(const Spec & spec);
  std::vector<Stage> stages_;
  std::vector<std::vector<std::size_t>> chains_;
};

// Takes the stage whole: unpacking it into Options would drop the chain's strictness.
Report apply_checked(const Stage & stage);

Report bind(const std::string & target, const std::string & group = "");

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

// Findings about the host as the whole file uses it: what no single stage's preflight can see.
Report preflight(const Spec & spec);

// The caller resolves `tid`: nothing in the file maps a stage to a kernel thread. A nonzero
// `window` also reads the thread's and its cpus' counters twice, `window` apart, and judges the
// difference.
Report verify(const Stage & stage, int tid, std::chrono::milliseconds window = {});

Report verify_process(const Spec & spec);

Report verify_process(const Spec & spec, int pid);

struct Binding
{
  std::string node;
  std::string group;
  std::string name;
  int pid = 0;
  int tid = 0;
};

std::vector<Binding> bindings();

namespace detail
{
// The daemon's record of `held`, threads of other processes, in daemon_dir(). A process that binds
// keeps its own record through apply_checked(stage).
std::string record_bindings(const std::vector<Binding> & held);

// `earlier`: the threads (tid, start time) the process had when the stage was bound. None of them
// is a child of the stage.
struct HeldStage
{
  Binding binding;
  std::vector<std::pair<int, std::uint64_t>> earlier;
};
std::vector<HeldStage> held_stages();

// preflight(spec) with every /proc and /sys path read under `root`, for tests on host snapshots.
Report preflight_at(const std::string & root, const Spec & spec);

// CYRA_DAEMON_DIR (default /run/cyra), created if missing. Throws std::runtime_error when it is
// not this user's or others can write it.
std::string daemon_dir();
// /proc/<pid>/task/<tid>/stat field 22, 0 when unreadable. Tells a thread from its tid's reuse.
std::uint64_t task_starttime(int pid, int tid);
}  // namespace detail

}  // namespace cyra

#endif  // CYRA_SPEC_HPP
