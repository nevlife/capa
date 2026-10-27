#include "component_manager.hpp"

#include "capa/flux.hpp"

#include <rclcpp/logging.hpp>

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>

namespace capa
{

ComponentManager::ComponentManager(std::string node_name)
: ::flux::ComponentManager(std::move(node_name)), spec_(Spec::load())
{
  if (spec_.empty()) RCLCPP_INFO(get_logger(), "capa: CAPA_SPEC is not set; no group is bound");
}

// Spec::declared() throws when a group or a stage is named twice, which refuses the load.
void ComponentManager::on_node_added(const NodeBase::SharedPtr & node)
{
  if (spec_.empty()) return;
  for (const auto & line : unbound_stages(spec_, *node)) {
    RCLCPP_WARN(get_logger(), "%s", line.c_str());
  }
}

// A refusal stops spin(), so the container exits with the reason, as capa::flux::bind() does.
void ComponentManager::on_thread_start(
  const NodeBase::SharedPtr & node, const rclcpp::CallbackGroup::SharedPtr & group)
{
  if (spec_.empty()) return;
  const Stage * stage = flux::declared(spec_, *node, group);
  if (stage == nullptr) return;
  const std::string name = detail::stage_name(*stage);
  Report rep;
  try {
    rep = apply_checked(*stage);
  } catch (const std::exception & e) {
    throw std::runtime_error("capa: stage " + name + ": " + e.what());
  }
  std::string warned;
  for (const auto & f : rep.findings) {
    if (f.verdict == Verdict::Warn) warned += (warned.empty() ? " (warn: " : ", ") + f.id;
  }
  if (!warned.empty()) warned += ")";
  RCLCPP_INFO(
    get_logger(), "capa: stage %s bound to thread %d%s", name.c_str(), this_tid(), warned.c_str());
}

std::vector<std::string> unbound_stages(
  const Spec & spec, rclcpp::node_interfaces::NodeBaseInterface & node)
{
  const std::string fqn = node.get_fully_qualified_name();
  std::vector<const Stage *> bound;
  for (const auto & entry : flux::detail::declared_groups(spec, node)) {
    bound.push_back(entry.second);
  }
  std::vector<std::string> out;
  for (const Stage & st : spec.stages()) {
    if (st.node != fqn || st.external || !st.match.empty()) continue;
    if (std::find(bound.begin(), bound.end(), &st) != bound.end()) continue;
    std::string why = "is named by its label alone, and no code in a container says a label";
    if (!st.holds.empty()) {
      why = "holds " + (st.holds.subscription.empty() ? st.holds.service : st.holds.subscription) +
            ", and no callback group of the node holds it";
    }
    out.push_back(
      "capa: stage " + detail::stage_name(st) + " of chain " + st.chain + " " + why +
      "; it binds no thread");
  }
  return out;
}

}  // namespace capa
