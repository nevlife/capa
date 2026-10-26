#include "capa/flux.hpp"
#include "capa/spec.hpp"
#include "flux/partitioned_executor.hpp"

#include <chrono>
#include <string>

// Compiled, never run. The C++ fences of docs/*/api.*.md are the [doc:] regions below, and
// scripts/check_doc_examples.py keeps the two the same.

void doc_apply(const capa::Spec & spec)
{
  // [doc:apply]
  capa::Options o;
  o.policy = capa::Policy::Fifo;       // Inherit / Other / Fifo / RoundRobin
  o.priority = 70;                     // 1..99 for Fifo/RoundRobin, else 0
  o.cpus = {3};                        // empty leaves affinity alone
  o.children = capa::Children::Reset;  // Reset / Inherit

  capa::Report rep = capa::apply_checked(o, capa::Strictness::Hard, /*control_priority=*/80);
  rep = capa::apply_checked(spec.stage("/perception_node", "infer"));  // a stage from a Spec
  rep = capa::bind("/perception_node", "infer");                       // the stage from CAPA_SPEC
  // [doc:/apply]
  (void)rep;
}

void doc_check(const capa::Spec & spec, const capa::Stage & stage, const capa::Options & o)
{
  // [doc:check]
  capa::Report host = capa::preflight(o, /*control_priority=*/80);  // judges, changes nothing
  capa::ThreadState self = capa::current();                         // this thread, from the kernel
  int tid = capa::this_tid();                                       // the kernel's id for it
  capa::ThreadState other = capa::observe(tid);                     // any thread, any process
  capa::Report drift = capa::verify(stage, tid);                    // a thread against its stage
  capa::Report stray = capa::verify_process(spec);                  // RT threads nothing explains
  capa::Report whole = capa::preflight(spec);                       // the whole file on this host
  capa::Report spent = capa::verify(stage, tid, std::chrono::seconds(1));  // and its counters
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
  flux::PartitionedExecutor & ex, const rclcpp::Node::SharedPtr & node,
  const rclcpp::CallbackGroup::SharedPtr & grab_group,
  const rclcpp::CallbackGroup::SharedPtr & infer_group,
  const rclcpp::CallbackGroup::SharedPtr & loop_group, const capa::Options & opts,
  const capa::Spec & spec, const std::string & name)
{
  // [doc:flux]
  capa::flux::bind(ex, *node, grab_group, "grab");  // the stage from CAPA_SPEC
  capa::flux::apply_checked(ex, *node, infer_group, opts, capa::Strictness::Hard);  // values
  capa::flux::apply_checked(ex, *node, loop_group, spec.stage(name, "loop"));  // a Spec's stage
  // [doc:/flux]
}
