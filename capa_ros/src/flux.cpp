#include "capa/flux.hpp"

#include <flux/detail/group_member.hpp>
#include <flux/subscription.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace capa::flux
{

namespace detail
{

// An rclcpp callback group has no name and does not know its node, so the pairing at this call
// is the only link between the file's (node, label) and a thread. It is checked here.
Result<rclcpp::CallbackGroup::SharedPtr> owned_group(
  rclcpp::Node & node, const rclcpp::CallbackGroup::SharedPtr & group, const char * caller)
{
  const auto base = node.get_node_base_interface();
  if (!group) {
    return base->get_default_callback_group();
  }
  bool owned = false;
  base->for_each_callback_group(
    [&](const rclcpp::CallbackGroup::SharedPtr & g) { owned = owned || g == group; });
  if (!owned) {
    return Error{
      ErrorKind::InvalidArgument, std::string("capa: ") + caller +
                                    ": the group does not belong to node " +
                                    base->get_fully_qualified_name()};
  }
  return group;
}

Result<rclcpp::CallbackGroup::SharedPtr> stage_group(
  rclcpp::Node & node, const rclcpp::CallbackGroup::SharedPtr & group, const Stage & stage)
{
  if (auto r = capa::detail::reject_external(stage, "flux::apply_checked()"); !r) {
    return r.error();
  }
  const std::string where = capa::detail::stage_name(stage);
  const std::string actual = node.get_fully_qualified_name();
  if (stage.node != actual) {
    return Error{
      ErrorKind::InvalidArgument, "capa: flux::apply_checked: stage " + where + " names node " +
                                    stage.node + " but was handed node " + actual};
  }
  auto owned = owned_group(node, group, "flux::apply_checked");
  if (!owned) {
    return owned;
  }
  const auto g = *std::move(owned);
  if (!stage.holds.empty()) {
    const GroupContent c = content(*node.get_node_base_interface(), g);
    const auto & names = stage.holds.subscription.empty() ? c.services : c.subscriptions;
    const auto & name =
      stage.holds.subscription.empty() ? stage.holds.service : stage.holds.subscription;
    if (std::find(names.begin(), names.end(), name) == names.end()) {
      return Error{
        ErrorKind::InvalidArgument, "capa: flux::apply_checked: stage " + where + " holds " + name +
                                      ", and the group handed over does not"};
    }
    return g;
  }
  const bool is_default = node.get_node_base_interface()->get_default_callback_group() == g;
  if (stage.group.empty() && !is_default) {
    return Error{
      ErrorKind::InvalidArgument,
      "capa: flux::apply_checked: stage " + where +
        " names no group, which the file means as the node's default callback group, but the "
        "group handed over is not that one"};
  }
  if (!stage.group.empty() && is_default) {
    return Error{
      ErrorKind::InvalidArgument,
      "capa: flux::apply_checked: stage " + where +
        " names a group, but the group handed over is the node's default one, which the file "
        "spells as an omitted label"};
  }
  return g;
}

namespace
{

std::vector<rclcpp::CallbackGroup::SharedPtr> groups_of(
  rclcpp::node_interfaces::NodeBaseInterface & node)
{
  std::vector<rclcpp::CallbackGroup::SharedPtr> out;
  node.for_each_callback_group(
    [&](const rclcpp::CallbackGroup::SharedPtr & g) { out.push_back(g); });
  return out;
}

Result<std::vector<const Stage *>> resolve(
  const Spec & spec, rclcpp::node_interfaces::NodeBaseInterface & node,
  const std::vector<rclcpp::CallbackGroup::SharedPtr> & groups)
{
  std::vector<GroupContent> contents;
  contents.reserve(groups.size());
  for (const auto & g : groups) {
    contents.push_back(content(node, g));
  }
  return spec.declared(node.get_fully_qualified_name(), contents);
}

}  // namespace

Result<std::vector<std::pair<rclcpp::CallbackGroup::SharedPtr, const Stage *>>> declared_groups(
  const Spec & spec, rclcpp::node_interfaces::NodeBaseInterface & node)
{
  const auto groups = groups_of(node);
  const auto resolved = resolve(spec, node, groups);
  if (!resolved) {
    return resolved.error();
  }
  const auto & stages = *resolved;
  std::vector<std::pair<rclcpp::CallbackGroup::SharedPtr, const Stage *>> out;
  for (std::size_t i = 0; i < groups.size(); ++i) {
    if (stages[i] != nullptr) {
      out.emplace_back(groups[i], stages[i]);
    }
  }
  return out;
}

}  // namespace detail

GroupContent content(
  rclcpp::node_interfaces::NodeBaseInterface & node, const rclcpp::CallbackGroup::SharedPtr & group)
{
  const auto g = group ? group : node.get_default_callback_group();
  GroupContent c;
  c.is_default = g == node.get_default_callback_group();
  g->collect_all_ptrs(
    [&](const rclcpp::SubscriptionBase::SharedPtr & s) {
      c.subscriptions.emplace_back(s->get_topic_name());
    },
    [&](const rclcpp::ServiceBase::SharedPtr & s) {
      c.services.emplace_back(s->get_service_name());
    },
    [](const rclcpp::ClientBase::SharedPtr &) {}, [](const rclcpp::TimerBase::SharedPtr &) {},
    [&](const rclcpp::Waitable::SharedPtr & w) {
      const auto member = std::dynamic_pointer_cast<::flux::detail::GroupMember>(w);
      if (!member) {
        return;
      }
      if (const auto * s = dynamic_cast<const ::flux::Subscription *>(&member->source())) {
        c.subscriptions.push_back(s->topic_name());
      }
    });
  return c;
}

Result<const Stage *> declared(
  const Spec & spec, rclcpp::node_interfaces::NodeBaseInterface & node,
  const rclcpp::CallbackGroup::SharedPtr & group)
{
  const auto g = group ? group : node.get_default_callback_group();
  const auto groups = detail::groups_of(node);
  const auto it = std::find(groups.begin(), groups.end(), g);
  if (it == groups.end()) {
    return Error{
      ErrorKind::InvalidArgument, std::string("capa: flux::declared: the group does not belong to "
                                              "node ") +
                                    node.get_fully_qualified_name()};
  }
  const auto stages = detail::resolve(spec, node, groups);
  if (!stages) {
    return stages.error();
  }
  return (*stages)[static_cast<std::size_t>(it - groups.begin())];
}

}  // namespace capa::flux
