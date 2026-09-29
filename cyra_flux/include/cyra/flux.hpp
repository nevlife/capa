#ifndef CYRA_FLUX_HPP
#define CYRA_FLUX_HPP

#include "cyra/rt.hpp"
#include "cyra/spec.hpp"

#include <flux/ros/partitioned_executor.hpp>
#include <rclcpp/node.hpp>

#include <string>

// Scheduling for the threads of a flux PartitionedExecutor. Each call is the cyra_core function
// of the same name, run by the child that serves `group` before its first callback; a refusal is
// the spin() error. `group` must belong to `node`, and nullptr means the node's default group.

namespace cyra::flux
{

// cyra::bind for the child serving `group`: the stage CYRA_SPEC declares for this node and
// `label`. An empty label names the node's default group. The stage is looked up here, so a typo
// throws now rather than at spin(). CYRA_SPEC unset registers nothing.
void bind(
  ::flux::ros::PartitionedExecutor & ex, rclcpp::Node & node,
  const rclcpp::CallbackGroup::SharedPtr & group, const std::string & label = "");

// cyra::apply_checked(opts, strict, control_priority) for the child serving `group`.
void apply_checked(
  ::flux::ros::PartitionedExecutor & ex, rclcpp::Node & node,
  const rclcpp::CallbackGroup::SharedPtr & group, const Options & opts,
  Strictness strict = Strictness::Soft, int control_priority = 0);

// cyra::apply_checked(stage) for the child serving `group`. The stage must name this node, and
// its label must match the group: empty for the default group, non-empty for any other.
void apply_checked(
  ::flux::ros::PartitionedExecutor & ex, rclcpp::Node & node,
  const rclcpp::CallbackGroup::SharedPtr & group, const Stage & stage);

}  // namespace cyra::flux

#endif  // CYRA_FLUX_HPP
