#ifndef CYRA_SPEC_HPP
#define CYRA_SPEC_HPP

#include "cyra/rt.hpp"

#include <string>
#include <vector>

// The chain declaration file: one place that says how every thread on a
// dataflow path must be scheduled, read by each process as it starts.
//
// Here rather than in a language wrapper so C++ and Python read the file through one parser. Two
// parsers would be two places for the ordering and collision rules to drift apart.

namespace cyra
{

// One thread on the chain.
//
// `external` marks a thread cyra cannot set: an rmw listener, a CUDA worker, a node handed to
// some other executor. Declared anyway, so a stage cyra merely omits and a stage cyra does not
// own do not look alike.
struct Stage
{
  std::string node;   // fully-qualified ROS node name
  std::string group;  // group label; empty means the node's default callback group
  bool external = false;
  int expect_priority = 0;  // external only: what the thread is expected to already run at

  Options opts;                          // what cyra applies (empty for external)
  Strictness strict = Strictness::Soft;  // from the chain's `target`
  int control_priority = 0;  // derived: the chain's last RT priority, 0 for the stage holding it

  std::string chain;  // which chain declared this, for error messages
};

namespace detail
{
// How the file spells a stage: `node`, or `node/group` for a named callback group.
std::string stage_name(const std::string & node, const std::string & group);
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

  // The stage declared for this fully-qualified node name and group label. Throws if the file
  // does not name it: applying nothing would hide a typo.
  const Stage & stage(const std::string & node, const std::string & group = "") const;

  // Same lookup without the throw; nullptr when absent.
  const Stage * find(const std::string & node, const std::string & group = "") const noexcept;

  // Stages cyra does not set. A caller that can resolve them to threads (a launch tool, an
  // operator) checks them; cyra only records what was declared.
  std::vector<const Stage *> external() const;

private:
  std::vector<Stage> stages_;
};

// Put a declared stage on the calling thread, whether an executor's group thread or a worker (an
// inference loop, a driver poll) no executor owns. Takes the stage whole: unpacking it into
// Options drops the chain's strictness and control priority. Rejects an external stage, and a
// stage another live thread of this process already holds.
Report apply_checked(const Stage & stage);

// Check a thread against what the file declared for it, without setting anything: the only
// thing cyra does for an `external` stage, and for the stages it sets the check that outlives
// the apply (`chrt`, systemd or a later apply can move a thread afterwards).
//
// `tid` is the thread to read (this_tid on the thread itself, or a tid the caller resolved).
// The file names stages by node and label, and nothing in it maps a label to a kernel thread.
Report verify(const Stage & stage, int tid);

// Every RT thread of this process the file cannot explain. A thread is explained when it runs at
// some stage's declared policy, priority and cpus, which is also what a callback's fork-join
// child holds, so a child needs no declaration of its own.
Report verify_process(const Spec & spec);

}  // namespace cyra

#endif  // CYRA_SPEC_HPP
