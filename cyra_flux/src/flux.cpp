#include "cyra/flux.hpp"

#include <stdexcept>
#include <string>

namespace cyra::flux
{

namespace
{

// An rclcpp callback group has no name and does not know its node, so the pairing at this call
// is the only link between the file's (node, label) and a thread. It is checked here.
rclcpp::CallbackGroup::SharedPtr owned_group(
  rclcpp::Node & node, const rclcpp::CallbackGroup::SharedPtr & group, const char * caller)
{
  const auto base = node.get_node_base_interface();
  if (!group) return base->get_default_callback_group();
  bool owned = false;
  base->for_each_callback_group(
    [&](const rclcpp::CallbackGroup::SharedPtr & g) { owned = owned || g == group; });
  if (!owned) {
    throw std::invalid_argument(
      std::string("cyra: ") + caller + ": the group does not belong to node " +
      base->get_fully_qualified_name());
  }
  return group;
}

}  // namespace

void bind(
  ::flux::ros::PartitionedExecutor & ex, rclcpp::Node & node,
  const rclcpp::CallbackGroup::SharedPtr & group, const std::string & label)
{
  const Spec spec = Spec::load();
  if (spec.empty()) return;
  apply_checked(ex, node, group, spec.stage(node.get_fully_qualified_name(), label));
}

void apply_checked(
  ::flux::ros::PartitionedExecutor & ex, rclcpp::Node & node,
  const rclcpp::CallbackGroup::SharedPtr & group, const Options & opts, Strictness strict,
  int control_priority)
{
  opts.validate();
  ex.on_thread_start(
    owned_group(node, group, "flux::apply_checked"),
    [opts, strict, control_priority] { cyra::apply_checked(opts, strict, control_priority); });
}

void apply_checked(
  ::flux::ros::PartitionedExecutor & ex, rclcpp::Node & node,
  const rclcpp::CallbackGroup::SharedPtr & group, const Stage & stage)
{
  detail::reject_external(stage, "flux::apply_checked()");
  const std::string where = detail::stage_name(stage);
  const std::string actual = node.get_fully_qualified_name();
  if (stage.node != actual) {
    throw std::invalid_argument(
      "cyra: flux::apply_checked: stage " + where + " names node " + stage.node +
      " but was handed node " + actual);
  }
  const auto g = owned_group(node, group, "flux::apply_checked");
  const bool is_default = node.get_node_base_interface()->get_default_callback_group() == g;
  if (stage.group.empty() && !is_default) {
    throw std::invalid_argument(
      "cyra: flux::apply_checked: stage " + where +
      " names no group, which the file means as the node's default callback group, but the "
      "group handed over is not that one");
  }
  if (!stage.group.empty() && is_default) {
    throw std::invalid_argument(
      "cyra: flux::apply_checked: stage " + where +
      " names a group, but the group handed over is the node's default one, which the file "
      "spells as an omitted label");
  }
  ex.on_thread_start(g, [stage] { cyra::apply_checked(stage); });
}

}  // namespace cyra::flux
