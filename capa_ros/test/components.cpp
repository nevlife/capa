// Components the container test loads. The groups are never named to capa: the chain file picks
// them by the default group and by what they hold.
#include <flux/subscription.hpp>
#include <rclcpp/rclcpp.hpp>
#include <rclcpp_components/register_node_macro.hpp>

#include <std_msgs/msg/u_int64.hpp>

#include <string>

namespace capa_ros_test
{

// odom in the default group, traj and map in groups of their own, and a flux subscription in a
// fourth group.
class Planner : public rclcpp::Node
{
public:
  explicit Planner(const rclcpp::NodeOptions & options)
  : Node("planner", options),
    traj_group_(create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive)),
    map_group_(create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive)),
    frames_group_(create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive)),
    odom_(subscribe("/capa_ct/odom", nullptr)),
    traj_(subscribe("/capa_ct/traj", traj_group_)),
    map_(subscribe("/capa_ct/map", map_group_)),
    frames_(*this, "/capa_ct/frames", flux::kNoSchema, flux::QoS{}, [](const flux::FrameView &) {})
  {
    frames_.add_to_callback_group(*this, frames_group_);
  }

private:
  rclcpp::Subscription<std_msgs::msg::UInt64>::SharedPtr subscribe(
    const std::string & topic, const rclcpp::CallbackGroup::SharedPtr & group)
  {
    rclcpp::SubscriptionOptions o;
    o.callback_group = group;
    return create_subscription<std_msgs::msg::UInt64>(
      topic, 10, [](const std_msgs::msg::UInt64 &) {}, o);
  }

  rclcpp::CallbackGroup::SharedPtr traj_group_;
  rclcpp::CallbackGroup::SharedPtr map_group_;
  rclcpp::CallbackGroup::SharedPtr frames_group_;
  rclcpp::Subscription<std_msgs::msg::UInt64>::SharedPtr odom_;
  rclcpp::Subscription<std_msgs::msg::UInt64>::SharedPtr traj_;
  rclcpp::Subscription<std_msgs::msg::UInt64>::SharedPtr map_;
  flux::Subscription frames_;
};

}  // namespace capa_ros_test

RCLCPP_COMPONENTS_REGISTER_NODE(capa_ros_test::Planner)
