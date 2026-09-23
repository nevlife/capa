#include "cyra/flux.hpp"

#include <stdexcept>
#include <string>

namespace cyra::flux
{

void schedule(
  ::flux::ros::PartitionedExecutor & ex, const rclcpp::CallbackGroup::SharedPtr & group,
  const Options & opts, Strictness strict, int control_priority)
{
  opts.validate();
  ex.on_thread_start(
    group, [opts, strict, control_priority] { apply_checked(opts, strict, control_priority); });
}

void schedule(
  ::flux::ros::PartitionedExecutor & ex, rclcpp::Node & node,
  const rclcpp::CallbackGroup::SharedPtr & group, const Stage & stage)
{
  detail::reject_external(stage, "schedule()");
  const std::string where = detail::stage_name(stage.node, stage.group);
  const auto base = node.get_node_base_interface();
  const std::string actual = base->get_fully_qualified_name();
  if (actual != stage.node) {
    throw std::invalid_argument(
      "cyra: schedule(node, group, stage): stage " + where + " names node " + stage.node +
      " but was handed node " + actual);
  }
  bool owned = false;
  base->for_each_callback_group(
    [&](const rclcpp::CallbackGroup::SharedPtr & g) { owned = owned || g == group; });
  if (!owned) {
    throw std::invalid_argument(
      "cyra: schedule(node, group, stage): stage " + where + " names node " + stage.node +
      " but the group belongs to another node");
  }
  const bool is_default = base->get_default_callback_group() == group;
  if (stage.group.empty() && !is_default) {
    throw std::invalid_argument(
      "cyra: schedule(node, group, stage): stage " + where +
      " names no group, which the file means as the node's default callback group, but the "
      "group handed over is not that one");
  }
  if (!stage.group.empty() && is_default) {
    throw std::invalid_argument(
      "cyra: schedule(node, group, stage): stage " + where +
      " names a group, but the group handed over is the node's default one, which the file "
      "spells as an omitted label");
  }
  ex.on_thread_start(group, [stage] { apply_checked(stage); });
}

}  // namespace cyra::flux
