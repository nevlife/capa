#ifndef CAPA_FLUX_HPP
#define CAPA_FLUX_HPP

#include "capa/rt.hpp"
#include "capa/spec.hpp"

#include <rclcpp/node.hpp>

#include <exception>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// Each call is the capa_core function of the same name, run by the child serving `group` before
// its first callback. `Executor` is a flux::PartitionedExecutor. A refusal on that child stops
// spin() with an exception, which is how flux hears of a hook that failed.

namespace capa::flux
{

namespace detail
{

Result<rclcpp::CallbackGroup::SharedPtr> owned_group(
  rclcpp::Node & node, const rclcpp::CallbackGroup::SharedPtr & group, const char * caller);

Result<rclcpp::CallbackGroup::SharedPtr> stage_group(
  rclcpp::Node & node, const rclcpp::CallbackGroup::SharedPtr & group, const Stage & stage);

// Every callback group of `node` the file declares, with its stage (Spec::declared).
Result<std::vector<std::pair<rclcpp::CallbackGroup::SharedPtr, const Stage *>>> declared_groups(
  const Spec & spec, rclcpp::node_interfaces::NodeBaseInterface & node);

// Registers `hook` for `group`. flux refuses a group that already has one by throwing.
template <class Executor, class Hook>
Result<void> on_start(Executor & ex, const rclcpp::CallbackGroup::SharedPtr & group, Hook && hook)
{
  try {
    ex.on_thread_start(group, std::forward<Hook>(hook));
  } catch (const std::logic_error & e) {
    return Error{ErrorKind::InvalidArgument, e.what()};
  } catch (const std::exception & e) {
    return Error{ErrorKind::Failed, e.what()};
  }
  return {};
}

}  // namespace detail

// What `group` (nullptr: the default one) of `node` holds, flux::Subscription objects included. A
// caller with an agnocast subscription or a flux message_filters::Subscriber adds its topic and
// calls Spec::declared().
GroupContent content(
  rclcpp::node_interfaces::NodeBaseInterface & node,
  const rclcpp::CallbackGroup::SharedPtr & group);

// The stage `spec` declares for `group` (nullptr: the default one) of `node`, or nullptr. For an
// executor that does not take hooks: call it on the group's thread and apply_checked() the result.
Result<const Stage *> declared(
  const Spec & spec, rclcpp::node_interfaces::NodeBaseInterface & node,
  const rclcpp::CallbackGroup::SharedPtr & group);

template <class Executor>
Result<void> apply_checked(
  Executor & ex, rclcpp::Node & node, const rclcpp::CallbackGroup::SharedPtr & group,
  const Options & opts, Strictness strict = Strictness::Soft, int control_priority = 0)
{
  if (auto valid = opts.validate(); !valid) {
    return valid;
  }
  auto owned = detail::owned_group(node, group, "flux::apply_checked");
  if (!owned) {
    return owned.error();
  }
  return detail::on_start(ex, *owned, [opts, strict, control_priority] {
    capa::detail::unwrap(capa::apply_checked(opts, strict, control_priority));
  });
}

template <class Executor>
Result<void> apply_checked(
  Executor & ex, rclcpp::Node & node, const rclcpp::CallbackGroup::SharedPtr & group,
  const Stage & stage)
{
  auto owned = detail::stage_group(node, group, stage);
  if (!owned) {
    return owned.error();
  }
  return detail::on_start(
    ex, *owned, [stage] { capa::detail::unwrap(capa::apply_checked(stage)); });
}

// The stage is looked up here, so a typo is an error now rather than at spin().
template <class Executor>
Result<void> bind(
  Executor & ex, rclcpp::Node & node, const rclcpp::CallbackGroup::SharedPtr & group,
  const std::string & label = "")
{
  const auto spec = Spec::load();
  if (!spec) {
    return spec.error();
  }
  if (spec->empty()) {
    return {};
  }
  const auto stage = spec->stage(node.get_fully_qualified_name(), label);
  if (!stage) {
    return stage.error();
  }
  return apply_checked(ex, node, group, **stage);
}

// bind() for every group of `node` that Spec::declared() resolves, for a container that loads
// nodes it knows nothing about: one call per node, before spin(). Groups created later are not
// seen, and a stage named by label alone is left to bind().
template <class Executor>
Result<void> bind_declared(Executor & ex, rclcpp::node_interfaces::NodeBaseInterface & node)
{
  const auto spec = Spec::load();
  if (!spec) {
    return spec.error();
  }
  if (spec->empty()) {
    return {};
  }
  const auto groups = detail::declared_groups(*spec, node);
  if (!groups) {
    return groups.error();
  }
  for (const auto & [group, stage] : *groups) {
    auto added =
      detail::on_start(ex, group, [st = *stage] { capa::detail::unwrap(capa::apply_checked(st)); });
    if (!added) {
      return added;
    }
  }
  return {};
}

}  // namespace capa::flux

#endif  // CAPA_FLUX_HPP
