#include "capa/flux.hpp"
#include "flux/partitioned_executor.hpp"
#include "flux/subscription.hpp"

#include <rclcpp/rclcpp.hpp>

#include "sensor_msgs/flux/image.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>

// The schedule is not in this file. A chain spans processes, and no node can see the ordering
// its own settings have to respect, so the declaration lives in one file and each node reads its
// own stage out of it. CAPA_SPEC names that file; unset leaves an empty spec and a no-op.
class StageNode : public rclcpp::Node
{
public:
  StageNode()
  : Node("capa_stage"),
    group_(create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive)),
    sub_(*this, kTopic, Image::kFingerprint, flux::QoS{}, [this](const flux::FrameView & f) {
      on_frame(f);
    })
  {
    timer_ = create_wall_timer(std::chrono::seconds(1), [this]() { report(); });
  }

  rclcpp::CallbackGroup::SharedPtr group() { return group_; }
  flux::Subscription & subscription() { return sub_; }

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
    const capa::ThreadState st = capa::current();
    RCLCPP_INFO(
      get_logger(), "seen %lu  this thread policy %d priority %d", seen_, st.policy, st.priority);
  }

  rclcpp::CallbackGroup::SharedPtr group_;
  flux::Subscription sub_;
  rclcpp::TimerBase::SharedPtr timer_;
  std::uint64_t seen_ = 0;
};

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  int rc = 0;
  try {
    auto node = std::make_shared<StageNode>();

    flux::PartitionedExecutor ex;
    ex.add_node(node);
    ex.add(node->subscription(), node->group());

    // The whole schedule of this thread is in the file CAPA_SPEC names; the code only says which
    // stage the group is. Unset, this registers nothing.
    capa::flux::bind(ex, *node, node->group(), StageNode::kGroupLabel);

    const capa::Spec spec = capa::Spec::load();
    if (spec.empty()) {
      RCLCPP_WARN(node->get_logger(), "CAPA_SPEC is unset: running with no declared schedule");
    }
    // The stages capa does not set are still part of the chain, and checking them is the only
    // thing it can do for them.
    for (const capa::Stage * e : spec.external()) {
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
