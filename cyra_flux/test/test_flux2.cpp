#include "cyra/flux.hpp"

#include <flux2/partitioned_executor.hpp>
#include <rclcpp/rclcpp.hpp>

#include <std_msgs/msg/u_int64.hpp>

#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

// The same functions over a flux2::PartitionedExecutor. Affinity needs no privilege, so each test
// reads the declared cpu set back from inside the callback, on the child thread that runs it.

namespace
{

struct Group
{
  std::shared_ptr<rclcpp::Node> node;
  rclcpp::CallbackGroup::SharedPtr group;
  rclcpp::Subscription<std_msgs::msg::UInt64>::SharedPtr sub;
  std::atomic<bool> seen{false};
  std::vector<std::uint32_t> cpus;

  Group(const std::string & name, const std::string & topic)
  : node(std::make_shared<rclcpp::Node>(name)),
    group(node->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive))
  {
    rclcpp::SubscriptionOptions opts;
    opts.callback_group = group;
    sub = node->create_subscription<std_msgs::msg::UInt64>(
      topic, 10,
      [this](const std_msgs::msg::UInt64 &) {
        if (!seen.exchange(true)) cpus = cyra::current().cpus;
      },
      opts);
  }
};

// Publishes until `done` or 5 s, with `ex` spinning on its own thread.
void spin_until(
  flux2::PartitionedExecutor & ex, const std::string & topic, const std::function<bool()> & done)
{
  auto pub_node = std::make_shared<rclcpp::Node>("cyra_flux2_pub");
  auto pub = pub_node->create_publisher<std_msgs::msg::UInt64>(topic, 10);
  std::thread spinner([&] { ex.spin(); });
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  std_msgs::msg::UInt64 m;
  while (!done() && std::chrono::steady_clock::now() < deadline) {
    pub->publish(m);
    std::this_thread::sleep_for(2ms);
  }
  ex.cancel();
  spinner.join();
}

}  // namespace

TEST(Flux2, ScheduleReachesTheChildThread)
{
  rclcpp::init(0, nullptr);
  Group g("cyra_flux2_sched", "/cyra_flux2/sched");
  flux2::PartitionedExecutor ex;
  ex.add_node(g.node);
  cyra::Options sched;
  sched.cpus = {0};
  cyra::flux::apply_checked(ex, *g.node, g.group, sched);

  spin_until(ex, "/cyra_flux2/sched", [&] { return g.seen.load(); });
  rclcpp::shutdown();

  ASSERT_TRUE(g.seen.load()) << "the scheduled group never received a message";
  EXPECT_EQ(g.cpus, (std::vector<std::uint32_t>{0}));
}

TEST(Flux2, BindReachesTheGroupChild)
{
  const std::string path = "/tmp/cyra_flux2_" + std::to_string(::getpid()) + ".yaml";
  {
    std::ofstream out(path);
    out << "chains:\n  c:\n    target: soft\n    stages:\n"
           "      - node: /cyra_flux2_bind\n        group: grab\n        cpus: [0]\n";
  }
  ::setenv("CYRA_SPEC", path.c_str(), 1);

  rclcpp::init(0, nullptr);
  Group g("cyra_flux2_bind", "/cyra_flux2/bind");
  flux2::PartitionedExecutor ex;
  ex.add_node(g.node);
  cyra::flux::bind(ex, *g.node, g.group, "grab");

  spin_until(ex, "/cyra_flux2/bind", [&] { return g.seen.load(); });
  rclcpp::shutdown();
  ::unsetenv("CYRA_SPEC");
  std::remove(path.c_str());

  ASSERT_TRUE(g.seen.load()) << "the bound group never received a message";
  EXPECT_EQ(g.cpus, (std::vector<std::uint32_t>{0}));
}

// A second declaration for a group, a group no child serves, and a declaration once spinning.
TEST(Flux2, ScheduleRejects)
{
  rclcpp::init(0, nullptr);
  auto node = std::make_shared<rclcpp::Node>("cyra_flux2_reject");
  auto g = node->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);

  flux2::PartitionedExecutor ex;
  ex.add_node(node);
  cyra::flux::apply_checked(ex, *node, g, cyra::Options{});
  EXPECT_THROW(cyra::flux::apply_checked(ex, *node, g, cyra::Options{}), std::invalid_argument);

  auto stray_node = std::make_shared<rclcpp::Node>("cyra_flux2_stray");
  auto stray = stray_node->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
  cyra::flux::apply_checked(ex, *stray_node, stray, cyra::Options{});
  EXPECT_THROW(ex.spin(), std::invalid_argument);

  flux2::PartitionedExecutor ex2;
  ex2.add_node(node);
  std::thread spinner([&] { ex2.spin(); });
  std::this_thread::sleep_for(100ms);
  EXPECT_THROW(cyra::flux::apply_checked(ex2, *node, g, cyra::Options{}), std::logic_error);
  ex2.cancel();
  spinner.join();
  rclcpp::shutdown();
}
