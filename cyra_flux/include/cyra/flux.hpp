#ifndef CYRA_FLUX_HPP
#define CYRA_FLUX_HPP

#include "cyra/rt.hpp"
#include "cyra/spec.hpp"

#include <rclcpp/node.hpp>

#include <string>

// Each call is the cyra_core function of the same name, run by the child serving `group` before
// its first callback. `Executor` is a flux::PartitionedExecutor.

namespace cyra::flux
{

namespace detail
{

rclcpp::CallbackGroup::SharedPtr owned_group(
  rclcpp::Node & node, const rclcpp::CallbackGroup::SharedPtr & group, const char * caller);

rclcpp::CallbackGroup::SharedPtr stage_group(
  rclcpp::Node & node, const rclcpp::CallbackGroup::SharedPtr & group, const Stage & stage);

}  // namespace detail

template <class Executor>
void apply_checked(
  Executor & ex, rclcpp::Node & node, const rclcpp::CallbackGroup::SharedPtr & group,
  const Options & opts, Strictness strict = Strictness::Soft, int control_priority = 0)
{
  opts.validate();
  ex.on_thread_start(
    detail::owned_group(node, group, "flux::apply_checked"),
    [opts, strict, control_priority] { cyra::apply_checked(opts, strict, control_priority); });
}

template <class Executor>
void apply_checked(
  Executor & ex, rclcpp::Node & node, const rclcpp::CallbackGroup::SharedPtr & group,
  const Stage & stage)
{
  ex.on_thread_start(
    detail::stage_group(node, group, stage), [stage] { cyra::apply_checked(stage); });
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

}  // namespace cyra::flux

#endif  // CYRA_FLUX_HPP
