#include "cyra/flux.hpp"
#include "flux/ros/partitioned_executor.hpp"
#include "flux/ros/subscription.hpp"

#include <rclcpp/rclcpp.hpp>

#include "sensor_msgs/flux/image.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>

// The schedule is not in this file. A chain spans processes, and no node can see the ordering
// its own settings have to respect, so the declaration lives in one file and each node reads its
// own stage out of it. CYRA_SPEC names that file; unset leaves an empty spec and a no-op.
class StageNode : public rclcpp::Node
{
public:
  StageNode()
  : Node("cyra_stage"),
    group_(create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive)),
    sub_(*this, kTopic, Image::kFingerprint, flux::QoS{}, [this](const flux::FrameView & f) {
      on_frame(f);
    })
  {
    timer_ = create_wall_timer(std::chrono::seconds(1), [this]() { report(); });
  }

  rclcpp::CallbackGroup::SharedPtr group() { return group_; }
  flux::ros::Subscription & subscription() { return sub_; }

  static constexpr const char * kGroupLabel = "work";

private:
  using Image = sensor_msgs::flux_msg::Image;

  static constexpr const char * kTopic = "image";

  void on_frame(const flux::FrameView & f)
  {
    Image::View v{f};
    if (v.ok__()) {
      ++seen_;
    }
  }

  void report()
  {
    const cyra::ThreadState st = cyra::current();
    RCLCPP_INFO(
      get_logger(), "seen %lu  this thread policy %d priority %d", seen_, st.policy, st.priority);
  }

  rclcpp::CallbackGroup::SharedPtr group_;
  flux::ros::Subscription sub_;
  rclcpp::TimerBase::SharedPtr timer_;
  std::uint64_t seen_ = 0;
};

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  int rc = 0;
  try {
    auto node = std::make_shared<StageNode>();

    flux::ros::PartitionedExecutor ex;
    ex.add_ros_node(node);
    ex.add(node->subscription(), node->group());

    // The whole schedule of this thread is in the file CYRA_SPEC names; the code only says which
    // stage the group is. Unset, this registers nothing.
    cyra::flux::bind(ex, *node, node->group(), StageNode::kGroupLabel);

    const cyra::Spec spec = cyra::Spec::load();
    if (spec.empty()) {
      RCLCPP_WARN(node->get_logger(), "CYRA_SPEC is unset: running with no declared schedule");
    }
    // The stages cyra does not set are still part of the chain, and checking them is the only
    // thing it can do for them.
    for (const cyra::Stage * e : spec.external()) {
      RCLCPP_INFO(
        node->get_logger(), "external stage %s expects priority %d", e->node.c_str(),
        e->expect_priority);
    }

    // A refused stage ends the spin with the reason rather than running at the wrong priority.
    ex.spin();
  } catch (const std::exception & e) {
    std::fprintf(stderr, "chain: %s\n", e.what());
    rc = 1;
  }
  rclcpp::shutdown();
  return rc;
}
