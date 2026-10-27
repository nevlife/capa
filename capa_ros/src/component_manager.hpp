#ifndef CAPA_ROS_COMPONENT_MANAGER_HPP
#define CAPA_ROS_COMPONENT_MANAGER_HPP

#include "capa/spec.hpp"

#include <flux/component_manager.hpp>

#include <string>
#include <vector>

namespace capa
{

// flux's container with CAPA_SPEC applied: each group thread binds the stage Spec::declared()
// gives its group before the first callback. A group with no stage runs as flux starts it.
class ComponentManager : public ::flux::ComponentManager
{
public:
  explicit ComponentManager(std::string node_name = "ComponentManager");

protected:
  void on_node_added(const NodeBase::SharedPtr & node) override;
  void on_thread_start(
    const NodeBase::SharedPtr & node, const rclcpp::CallbackGroup::SharedPtr & group) override;

private:
  Spec spec_;
};

// One line for each stage of `node` that the container binds to no group: a `holds` that no group
// of the node holds, or a stage named by label alone, which no code in a container says.
std::vector<std::string> unbound_stages(
  const Spec & spec, rclcpp::node_interfaces::NodeBaseInterface & node);

}  // namespace capa

#endif  // CAPA_ROS_COMPONENT_MANAGER_HPP
