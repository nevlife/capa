#ifndef CAPA_FLUX_HPP
#define CAPA_FLUX_HPP

#include "capa/rt.hpp"
#include "capa/spec.hpp"

#include <rclcpp/node.hpp>

#include <string>
#include <utility>
#include <vector>

// Each call is the capa_core function of the same name, run by the child serving `group` before
// its first callback. `Executor` is a flux::PartitionedExecutor.

namespace capa::flux
{

namespace detail
{

rclcpp::CallbackGroup::SharedPtr owned_group(
  rclcpp::Node & node, const rclcpp::CallbackGroup::SharedPtr & group, const char * caller);

rclcpp::CallbackGroup::SharedPtr stage_group(
  rclcpp::Node & node, const rclcpp::CallbackGroup::SharedPtr & group, const Stage & stage);

// Every callback group of `node` the file declares, with its stage (Spec::declared).
std::vector<std::pair<rclcpp::CallbackGroup::SharedPtr, const Stage *>> declared_groups(
  const Spec & spec, rclcpp::node_interfaces::NodeBaseInterface & node);

}  // namespace detail

// What `group` (nullptr: the default one) of `node` holds, flux::Subscription objects included. A
// caller with an agnocast subscription or a flux message_filters::Subscriber adds its topic and
// calls Spec::declared().
GroupContent content(
  rclcpp::node_interfaces::NodeBaseInterface & node,
  const rclcpp::CallbackGroup::SharedPtr & group);

// The stage `spec` declares for `group` (nullptr: the default one) of `node`, or nullptr. For an
// executor that does not take hooks: call it on the group's thread and apply_checked() the result.
const Stage * declared(
  const Spec & spec, rclcpp::node_interfaces::NodeBaseInterface & node,
  const rclcpp::CallbackGroup::SharedPtr & group);

template <class Executor>
void apply_checked(
  Executor & ex, rclcpp::Node & node, const rclcpp::CallbackGroup::SharedPtr & group,
  const Options & opts, Strictness strict = Strictness::Soft, int control_priority = 0)
{
  opts.validate();
  ex.on_thread_start(
    detail::owned_group(node, group, "flux::apply_checked"),
    [opts, strict, control_priority] { capa::apply_checked(opts, strict, control_priority); });
}

template <class Executor>
void apply_checked(
  Executor & ex, rclcpp::Node & node, const rclcpp::CallbackGroup::SharedPtr & group,
  const Stage & stage)
{
  ex.on_thread_start(
    detail::stage_group(node, group, stage), [stage] { capa::apply_checked(stage); });
}

// The stage is looked up here, so a typo throws now rather than at spin().
template <class Executor>
void bind(
  Executor & ex, rclcpp::Node & node, const rclcpp::CallbackGroup::SharedPtr & group,
  const std::string & label = "")
{
  const Spec spec = Spec::load();
  if (spec.empty()) return;
  apply_checked(ex, node, group, spec.stage(node.get_fully_qualified_name(), label));
}

// bind() for every group of `node` that Spec::declared() resolves, for a container that loads
// nodes it knows nothing about: one call per node, before spin(). Groups created later are not
// seen, and a stage named by label alone is left to bind().
template <class Executor>
void bind_declared(Executor & ex, rclcpp::node_interfaces::NodeBaseInterface & node)
{
  const Spec spec = Spec::load();
  if (spec.empty()) return;
  for (const auto & [group, stage] : detail::declared_groups(spec, node)) {
    ex.on_thread_start(group, [st = *stage] { capa::apply_checked(st); });
  }
}

}  // namespace capa::flux

#endif  // CAPA_FLUX_HPP
