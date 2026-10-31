#ifndef CAPA_SPEC_HPP
#define CAPA_SPEC_HPP

#include "capa/rt.hpp"

#include <chrono>
#include <cstddef>
#include <exception>
#include <future>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// The one parser for C++ and Python, so the file rules cannot drift between languages.

namespace capa
{

enum class Kind : std::uint8_t { Worker = 0, Release };

struct Match
{
  std::string irq;      // /sys/kernel/irq/<n>/actions, e.g. "3550000.usb"
  std::string process;  // /proc/<pid>/comm of the owning process
  std::string arg;      // one whole element of /proc/<pid>/cmdline of the owning process
  std::string thread;   // /proc/<pid>/task/<tid>/comm

  [[nodiscard]] auto empty() const noexcept -> bool
  {
    return irq.empty() && process.empty() && arg.empty() && thread.empty();
  }
};

// Names a callback group by what it holds, for groups that have no name of their own.
struct Holds
{
  std::string subscription;  // resolved topic name
  std::string service;       // resolved service name

  [[nodiscard]] auto empty() const noexcept -> bool
  {
    return subscription.empty() && service.empty();
  }
};

// What one callback group of a node holds, as Spec::declared() reads it.
struct GroupContent
{
  bool is_default = false;
  std::vector<std::string> subscriptions;
  std::vector<std::string> services;
};

struct Stage
{
  std::string node;   // fully-qualified ROS node name; empty for a `name` stage
  std::string group;  // group label; empty means the node's default callback group
  Holds holds;        // set: the group is the one of `node` holding this
  std::string name;   // a non-ROS thread; empty for a `node` stage
  Kind kind = Kind::Worker;
  bool external = false;
  int expect_priority = 0;  // external only: what the thread is expected to already run at

  Match match;  // set: the daemon finds and sets this thread, and bind() refuses it
  std::vector<std::uint32_t> children_cpus;  // the daemon moves reset children here

  Options opts;                          // what capa applies (empty for external)
  Strictness strict = Strictness::Soft;  // the strictest `target` among the chains it sits on

  std::string chain;  // which chain declared this, for error messages
};

class Spec;

namespace detail
{
// How messages spell a stage: `node`, `node/group`, or `name`.
auto stage_name(const Stage & stage) -> std::string;
void reject_external(const Stage & stage, const char * caller);
// Each chain's stages, as indices into stages(), in flow order.
auto chains(const Spec & spec) -> const std::vector<std::vector<std::size_t>> &;
}  // namespace detail

class Spec
{
public:
  Spec() = default;

  static auto load(const std::string & path) -> Spec;

  static auto load() -> Spec;

  [[nodiscard]] auto empty() const noexcept -> bool { return stages_.empty(); }
  [[nodiscard]] auto stages() const noexcept -> const std::vector<Stage> & { return stages_; }

  [[nodiscard]] auto stage(const std::string & target, const std::string & group = "") const
    -> const Stage &;

  [[nodiscard]] auto find(const std::string & target, const std::string & group = "") const noexcept
    -> const Stage *;

  [[nodiscard]] auto external() const -> std::vector<const Stage *>;

  // For each of `groups`, every callback group of `node`, the stage the file declares for it, or
  // nullptr: a stage with no group for the default one, a `holds` stage for the group that holds
  // its subscription or service. A stage named by label alone, an external one and a matched one
  // are not resolved here. Throws when one group is claimed twice or one stage by two groups.
  [[nodiscard]] auto declared(const std::string & node, const std::vector<GroupContent> & groups)
    const -> std::vector<const Stage *>;

private:
  friend auto detail::chains(const Spec & spec) -> const std::vector<std::vector<std::size_t>> &;
  std::vector<Stage> stages_;
  std::vector<std::vector<std::size_t>> chains_;
};

// Takes the stage whole: unpacking it into Options would drop the chain's strictness.
auto apply_checked(const Stage & stage) -> Report;

auto bind(const std::string & target, const std::string & group = "") -> Report;

// bind() for a caller that binds every thread it starts: nullopt, and nothing applied, when the
// file declares no stage there that a thread binds itself (none, external, or matched).
auto bind_declared(const std::string & target, const std::string & group = "")
  -> std::optional<Report>;

template <class F>
auto spawn(const std::string & target, const std::string & group, F && fn) -> std::thread
{
  std::promise<void> ready;
  auto bound = ready.get_future();
  // Owned by the thread: the caller may return and drop its future while set_value still runs.
  std::thread t([ready = std::move(ready), target, group, fn = std::forward<F>(fn)]() mutable {
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
auto preflight(const Spec & spec) -> Report;

// The caller resolves `tid`: nothing in the file maps a stage to a kernel thread. A nonzero
// `window` also reads the thread's and its cpus' counters twice, `window` apart, and judges the
// difference.
auto verify(const Stage & stage, int tid, std::chrono::milliseconds window = {}) -> Report;

auto verify_process(const Spec & spec) -> Report;

auto verify_process(const Spec & spec, int pid) -> Report;

struct Binding
{
  std::string node;
  std::string group;
  std::string name;
  int pid = 0;
  int tid = 0;
};

auto bindings() -> std::vector<Binding>;

namespace detail
{
// The daemon's record of `held`, threads of other processes, in daemon_dir(). A process that binds
// keeps its own record through apply_checked(stage).
auto record_bindings(const std::vector<Binding> & held) -> std::string;

// `earlier`: the threads (tid, start time) the process had when the stage was bound. None of them
// is a child of the stage.
struct HeldStage
{
  Binding binding;
  std::vector<std::pair<int, std::uint64_t>> earlier;
  bool ask = false;  // the thread may not set itself and asks the daemon to
};
auto held_stages() -> std::vector<HeldStage>;

// preflight(spec) with every /proc and /sys path read under `root`, for tests on host snapshots.
auto preflight_at(const std::string & root, const Spec & spec) -> Report;
// The findings verify() reads from /proc about thread `tid` in state `st`, under `root`.
auto verify_now_at(const std::string & root, int tid, const ThreadState & st) -> Report;

// CAPA_DAEMON_DIR (default /run/capa), created if missing. Throws std::runtime_error when it is
// not this user's or others can write it.
auto daemon_dir() -> std::string;
// /proc/<pid>/task/<tid>/stat field 22, 0 when unreadable. Tells a thread from its tid's reuse.
auto task_starttime(int pid, int tid) -> std::uint64_t;
}  // namespace detail

}  // namespace capa

#endif  // CAPA_SPEC_HPP
