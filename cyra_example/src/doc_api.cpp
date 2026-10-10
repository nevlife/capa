#include "cyra/flux.hpp"
#include "cyra/spec.hpp"
#include "flux/ros/partitioned_executor.hpp"

#include <chrono>
#include <string>

// Compiled, never run. The C++ fences of docs/*/api.*.md are the [doc:] regions below, and
// scripts/check_doc_examples.py keeps the two the same.

void doc_apply(const cyra::Spec & spec)
{
  // [doc:apply]
  cyra::Options o;
  o.policy = cyra::Policy::Fifo;       // Inherit / Other / Fifo / RoundRobin
  o.priority = 70;                     // 1..99 for Fifo/RoundRobin, else 0
  o.cpus = {3};                        // empty leaves affinity alone
  o.children = cyra::Children::Reset;  // Reset / Inherit

  cyra::Report rep = cyra::apply_checked(o, cyra::Strictness::Hard, /*control_priority=*/80);
  rep = cyra::apply_checked(spec.stage("/perception_node", "infer"));  // a stage from a Spec
  rep = cyra::bind("/perception_node", "infer");                       // the stage from CYRA_SPEC
  // [doc:/apply]
  (void)rep;
}

void doc_check(const cyra::Spec & spec, const cyra::Stage & stage, const cyra::Options & o)
{
  // [doc:check]
  cyra::Report host = cyra::preflight(o, /*control_priority=*/80);  // judges, changes nothing
  cyra::ThreadState self = cyra::current();                         // this thread, from the kernel
  int tid = cyra::this_tid();                                       // the kernel's id for it
  cyra::ThreadState other = cyra::observe(tid);                     // any thread, any process
  cyra::Report drift = cyra::verify(stage, tid);                    // a thread against its stage
  cyra::Report stray = cyra::verify_process(spec);                  // RT threads nothing explains
  cyra::Report whole = cyra::preflight(spec);                       // the whole file on this host
  cyra::Report spent = cyra::verify(stage, tid, std::chrono::seconds(1));  // and its counters
  // [doc:/check]
  (void)host;
  (void)self;
  (void)other;
  (void)drift;
  (void)stray;
  (void)whole;
  (void)spent;
}

void doc_flux(
  flux::ros::PartitionedExecutor & ex, const rclcpp::Node::SharedPtr & node,
  const rclcpp::CallbackGroup::SharedPtr & grab_group,
  const rclcpp::CallbackGroup::SharedPtr & infer_group,
  const rclcpp::CallbackGroup::SharedPtr & loop_group, const cyra::Options & opts,
  const cyra::Spec & spec, const std::string & name)
{
  // [doc:flux]
  cyra::flux::bind(ex, *node, grab_group, "grab");  // the stage from CYRA_SPEC
  cyra::flux::apply_checked(ex, *node, infer_group, opts, cyra::Strictness::Hard);  // values
  cyra::flux::apply_checked(ex, *node, loop_group, spec.stage(name, "loop"));  // a Spec's stage
  // [doc:/flux]
}
