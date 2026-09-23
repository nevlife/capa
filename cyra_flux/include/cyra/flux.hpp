#ifndef CYRA_FLUX_HPP
#define CYRA_FLUX_HPP

#include "cyra/rt.hpp"
#include "cyra/spec.hpp"

#include <flux/ros/partitioned_executor.hpp>
#include <rclcpp/node.hpp>

// Scheduling for the threads of a flux PartitionedExecutor. Each call registers an
// on_thread_start() hook, so the child serving the group applies the declaration to itself
// before its first callback, and a refusal is the spin() error.

namespace cyra::flux
{

// `opts` for the child serving `group`. `strict` decides what a preflight Warn does;
// `control_priority` is the consumer's control loop priority, 0 when undeclared.
void schedule(
  ::flux::ros::PartitionedExecutor & ex, const rclcpp::CallbackGroup::SharedPtr & group,
  const Options & opts, Strictness strict = Strictness::Soft, int control_priority = 0);

// `stage` for the child serving `group`. An rclcpp callback group has no name, so the pairing
// here is the only link between the file's (node, label) and the thread. Checked on the spot:
// the group belongs to `node`, `node` is the one the stage names, and an empty label means that
// node's default callback group. Rejects an external stage.
void schedule(
  ::flux::ros::PartitionedExecutor & ex, rclcpp::Node & node,
  const rclcpp::CallbackGroup::SharedPtr & group, const Stage & stage);

}  // namespace cyra::flux

#endif  // CYRA_FLUX_HPP
