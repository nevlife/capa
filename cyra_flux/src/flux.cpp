#include "cyra/flux.hpp"

#include <stdexcept>
#include <string>

namespace cyra::flux
{

namespace detail
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

rclcpp::CallbackGroup::SharedPtr stage_group(
  rclcpp::Node & node, const rclcpp::CallbackGroup::SharedPtr & group, const Stage & stage)
{
  cyra::detail::reject_external(stage, "flux::apply_checked()");
  const std::string where = cyra::detail::stage_name(stage);
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
  return g;
}

}  // namespace detail

}  // namespace cyra::flux
