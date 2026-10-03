#ifndef CYRA_FLUX_HPP
#define CYRA_FLUX_HPP

#include "cyra/rt.hpp"
#include "cyra/spec.hpp"

#include <flux/ros/partitioned_executor.hpp>
#include <rclcpp/node.hpp>

#include <string>

// Each call is the cyra_core function of the same name, run by the child serving `group` before
// its first callback.

namespace cyra::flux
{

// The stage is looked up here, so a typo throws now rather than at spin().
void bind(
  ::flux::ros::PartitionedExecutor & ex, rclcpp::Node & node,
  const rclcpp::CallbackGroup::SharedPtr & group, const std::string & label = "");

void apply_checked(
  ::flux::ros::PartitionedExecutor & ex, rclcpp::Node & node,
  const rclcpp::CallbackGroup::SharedPtr & group, const Options & opts,
  Strictness strict = Strictness::Soft, int control_priority = 0);

void apply_checked(
  ::flux::ros::PartitionedExecutor & ex, rclcpp::Node & node,
  const rclcpp::CallbackGroup::SharedPtr & group, const Stage & stage);

}  // namespace cyra::flux

#endif  // CYRA_FLUX_HPP
