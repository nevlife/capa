#include "cyra/flux.hpp"

#include <rclcpp/rclcpp.hpp>

#include <std_msgs/msg/u_int64.hpp>

#include <gtest/gtest.h>
#include <sched.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

// cyra::flux::schedule(): the child thread serving the group applies the declared scheduling to
// itself before its first callback. Affinity needs no privilege, so it proves end to end that the
// options reach the child thread: the callback reads its own affinity back from the kernel.
TEST(Schedule, ScheduleReachesTheChildThread)
{
  rclcpp::init(0, nullptr);
  auto pub_node = std::make_shared<rclcpp::Node>("cie_sched_pub");
  auto sub_node = std::make_shared<rclcpp::Node>("cie_sched_sub");

  auto g = sub_node->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
  auto ros_pub = pub_node->create_publisher<std_msgs::msg::UInt64>("/part/sched", 10);

  rclcpp::SubscriptionOptions opts;
  opts.callback_group = g;
  std::atomic<bool> seen{false};
  std::vector<std::uint32_t> cpus_in_callback;
  auto ros_sub = sub_node->create_subscription<std_msgs::msg::UInt64>(
    "/part/sched", 10,
    [&](const std_msgs::msg::UInt64 &) {
      if (!seen.exchange(true)) cpus_in_callback = cyra::current().cpus;
    },
    opts);

  flux::ros::PartitionedExecutor ex;
  ex.add_ros_node(sub_node);
  cyra::Options sched;
  sched.cpus = {0};
  cyra::flux::schedule(ex, g, sched);

  std::atomic<bool> run{true};
  std::thread spinner([&] { ex.spin(run, 20'000'000); });

  const auto deadline = std::chrono::steady_clock::now() + 5s;
  std_msgs::msg::UInt64 m;
  while (!seen.load() && std::chrono::steady_clock::now() < deadline) {
    ros_pub->publish(m);
    std::this_thread::sleep_for(2ms);
  }

  run.store(false);
  ex.stop();
  spinner.join();
  rclcpp::shutdown();

  ASSERT_TRUE(seen.load()) << "the scheduled group never received a message";
  EXPECT_EQ(cpus_in_callback, (std::vector<std::uint32_t>{0}))
    << "the callback did not run on the pinned cpu set";
}

// An RT-policy schedule follows the cyra::apply contract through the executor: where preflight
// says the policy is permitted the child thread must actually hold it, and where it is not the
// refusal must surface as the spin() error -- never as a thread silently left at the default.
TEST(Schedule, ScheduleFifoMatchesPreflight)
{
  cyra::Options sched;
  sched.policy = cyra::Policy::Fifo;
  sched.priority = 1;
  const cyra::Report rep = cyra::preflight(sched);
  const cyra::Finding * perm = rep.find("policy-permission");
  ASSERT_NE(perm, nullptr);

  rclcpp::init(0, nullptr);
  auto pub_node = std::make_shared<rclcpp::Node>("cie_fifo_pub");
  auto sub_node = std::make_shared<rclcpp::Node>("cie_fifo_sub");

  auto g = sub_node->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
  auto ros_pub = pub_node->create_publisher<std_msgs::msg::UInt64>("/part/fifo", 10);

  rclcpp::SubscriptionOptions opts;
  opts.callback_group = g;
  std::atomic<bool> seen{false};
  int policy_in_callback = -1;
  auto ros_sub = sub_node->create_subscription<std_msgs::msg::UInt64>(
    "/part/fifo", 10,
    [&](const std_msgs::msg::UInt64 &) {
      if (!seen.exchange(true)) policy_in_callback = cyra::current().policy;
    },
    opts);

  flux::ros::PartitionedExecutor ex;
  ex.add_ros_node(sub_node);
  cyra::flux::schedule(ex, g, sched);

  std::atomic<bool> run{true};
  if (perm->verdict != cyra::Verdict::Ok) {
    // std::runtime_error, not system_error: the child goes through cyra::apply_checked, so the
    // refusal now comes from preflight before the thread is touched rather than from the
    // syscall afterwards. system_error derives from runtime_error, so this covers both.
    EXPECT_THROW(ex.spin(run, 20'000'000), std::runtime_error);
    rclcpp::shutdown();
    return;
  }

  std::thread spinner([&] { ex.spin(run, 20'000'000); });
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  std_msgs::msg::UInt64 m;
  while (!seen.load() && std::chrono::steady_clock::now() < deadline) {
    ros_pub->publish(m);
    std::this_thread::sleep_for(2ms);
  }
  run.store(false);
  ex.stop();
  spinner.join();
  rclcpp::shutdown();

  ASSERT_TRUE(seen.load()) << "the scheduled group never received a message";
  EXPECT_EQ(policy_in_callback, SCHED_FIFO) << "preflight said Ok but the child is not FIFO";
}

// The schedule surface rejects everything it cannot honor: a null group, options validate()
// refuses, a second schedule for the same group, a schedule for a group no child will serve,
// and any schedule() once spinning.
TEST(Schedule, ScheduleRejects)
{
  rclcpp::init(0, nullptr);
  auto node = std::make_shared<rclcpp::Node>("cie_sched_reject");

  flux::ros::PartitionedExecutor ex;
  ex.add_ros_node(node);

  EXPECT_THROW(cyra::flux::schedule(ex, nullptr, cyra::Options{}), std::invalid_argument);

  auto g = node->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
  cyra::Options bad;
  bad.policy = cyra::Policy::Fifo;  // priority 0 with an RT policy: reinterpretable
  EXPECT_THROW(cyra::flux::schedule(ex, g, bad), std::invalid_argument);

  cyra::flux::schedule(ex, g, cyra::Options{});
  EXPECT_THROW(
    cyra::flux::schedule(ex, g, cyra::Options{}), std::invalid_argument);  // one schedule per group

  // A group of a node never handed over: no child will serve it, so the schedule would
  // silently never apply. spin() refuses to start.
  auto stray_node = std::make_shared<rclcpp::Node>("cie_sched_stray");
  auto stray = stray_node->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
  cyra::flux::schedule(ex, stray, cyra::Options{});
  std::atomic<bool> run{true};
  EXPECT_THROW(ex.spin(run, 20'000'000), std::invalid_argument);

  flux::ros::PartitionedExecutor ex2;
  ex2.add_ros_node(node);
  std::thread spinner([&] { ex2.spin(run, 20'000'000); });
  std::this_thread::sleep_for(100ms);
  EXPECT_THROW(cyra::flux::schedule(ex2, g, cyra::Options{}), std::logic_error);
  run.store(false);
  ex2.stop();
  spinner.join();
  rclcpp::shutdown();
}

// The child applies through cyra::apply_checked, so a host that cannot hold a deadline is refused
// when the caller declared Hard rather than surfacing as a thread that quietly runs non-RT.
TEST(Schedule, HardStrictnessSurfacesAsASpinError)
{
  rclcpp::init(0, nullptr);
  auto node = std::make_shared<rclcpp::Node>("cie_strict");

  cyra::Options opts;
  opts.cpus = {0};
  const auto rep = cyra::preflight(opts);
  bool warned = false;
  for (const auto & f : rep.findings) {
    if (f.verdict == cyra::Verdict::Warn) warned = true;
  }
  if (!rep.ok() || !warned) {
    rclcpp::shutdown();
    GTEST_SKIP() << "cyra-cap:rt-warn-verdict this host has no Warn-only verdict to strengthen";
  }

  flux::ros::PartitionedExecutor ex;
  ex.add_ros_node(node);
  auto g = node->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
  cyra::flux::schedule(ex, g, opts, cyra::Strictness::Hard);

  std::atomic<bool> run{true};
  EXPECT_THROW(ex.spin(run, 20'000'000), std::runtime_error);

  rclcpp::shutdown();
}

// ---- the declaration file's name for a thread, checked against the group handed over ----
//
// A stage names its thread (node, label); an rclcpp callback group has no name at all. The only
// thing linking the two is the pairing at the schedule() call.

namespace
{

class SpecFile
{
public:
  explicit SpecFile(const std::string & body)
  : path_(
      "/tmp/cyra_flux_spec_" + std::to_string(::getpid()) + "_" + std::to_string(++counter_) +
      ".yaml")
  {
    std::ofstream out(path_);
    out << body;
  }
  ~SpecFile() { std::remove(path_.c_str()); }
  const std::string & path() const { return path_; }

private:
  static int counter_;
  std::string path_;
};
int SpecFile::counter_ = 0;

template <typename F>
std::string refusal(F && call)
{
  try {
    call();
  } catch (const std::invalid_argument & e) {
    return e.what();
  } catch (...) {
    return "<wrong exception type>";
  }
  return "<no exception>";
}

// Affinity only: these cases are about which thread a stage names, and applying a real RT policy
// would need a privilege the check does not.
constexpr const char * kLabelSpec = R"(
chains:
  c:
    target: soft
    stages:
      - node: /cie_label
        group: infer
        cpus: [0]
      - node: /cie_label_other
        cpus: [0]
)";

}  // namespace

TEST(ScheduleStage, RejectsAStageWhoseNodeIsNotTheNodeHandedOver)
{
  rclcpp::init(0, nullptr);
  auto node = std::make_shared<rclcpp::Node>("cie_label");
  SpecFile f(kLabelSpec);
  const auto spec = cyra::Spec::load(f.path());

  auto g = node->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
  flux::ros::PartitionedExecutor ex;
  const std::string why =
    refusal([&] { cyra::flux::schedule(ex, *node, g, *spec.find("/cie_label_other")); });
  rclcpp::shutdown();

  EXPECT_NE(why.find("but was handed node"), std::string::npos) << why;
}

TEST(ScheduleStage, RejectsAGroupOfAnotherNode)
{
  rclcpp::init(0, nullptr);
  auto node = std::make_shared<rclcpp::Node>("cie_label");
  auto other = std::make_shared<rclcpp::Node>("cie_label_other");
  SpecFile f(kLabelSpec);
  const auto spec = cyra::Spec::load(f.path());

  auto g = other->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
  flux::ros::PartitionedExecutor ex;
  const std::string why =
    refusal([&] { cyra::flux::schedule(ex, *node, g, *spec.find("/cie_label", "infer")); });
  rclcpp::shutdown();

  EXPECT_NE(why.find("the group belongs to another node"), std::string::npos) << why;
}

TEST(ScheduleStage, RejectsALabelledStageOnTheDefaultGroup)
{
  rclcpp::init(0, nullptr);
  auto node = std::make_shared<rclcpp::Node>("cie_label");
  SpecFile f(kLabelSpec);
  const auto spec = cyra::Spec::load(f.path());

  flux::ros::PartitionedExecutor ex;
  const std::string why = refusal([&] {
    cyra::flux::schedule(
      ex, *node, node->get_node_base_interface()->get_default_callback_group(),
      *spec.find("/cie_label", "infer"));
  });
  rclcpp::shutdown();

  EXPECT_NE(why.find("the node's default one"), std::string::npos) << why;
}

TEST(ScheduleStage, RejectsAnUnlabelledStageOnANamedGroup)
{
  rclcpp::init(0, nullptr);
  auto node = std::make_shared<rclcpp::Node>("cie_label_other");
  SpecFile f(kLabelSpec);
  const auto spec = cyra::Spec::load(f.path());

  auto g = node->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
  flux::ros::PartitionedExecutor ex;
  const std::string why =
    refusal([&] { cyra::flux::schedule(ex, *node, g, *spec.find("/cie_label_other")); });
  rclcpp::shutdown();

  EXPECT_NE(why.find("names no group"), std::string::npos) << why;
}

TEST(ScheduleStage, AcceptsAStageOnTheGroupItNames)
{
  rclcpp::init(0, nullptr);
  auto node = std::make_shared<rclcpp::Node>("cie_label");
  SpecFile f(kLabelSpec);
  const auto spec = cyra::Spec::load(f.path());

  auto g = node->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
  flux::ros::PartitionedExecutor ex;
  ex.add_ros_node(node);
  cyra::flux::schedule(ex, *node, g, *spec.find("/cie_label", "infer"));

  std::atomic<bool> run{true};
  std::thread spinner([&] { ex.spin(run, 20'000'000); });
  std::this_thread::sleep_for(100ms);
  ex.stop();
  spinner.join();
  rclcpp::shutdown();
}

// One stage is one thread. Handing it to two groups would leave the file describing neither, and
// the second child to apply it is the spin() error.
TEST(ScheduleStage, RejectsOneStageOnTwoGroups)
{
  rclcpp::init(0, nullptr);
  auto node = std::make_shared<rclcpp::Node>("cie_label");
  SpecFile f(kLabelSpec);
  const auto spec = cyra::Spec::load(f.path());

  auto ga = node->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
  auto gb = node->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
  flux::ros::PartitionedExecutor ex;
  ex.add_ros_node(node);
  const auto & stage = *spec.find("/cie_label", "infer");
  cyra::flux::schedule(ex, *node, ga, stage);
  cyra::flux::schedule(ex, *node, gb, stage);

  std::atomic<bool> run{true};
  const std::string why = refusal([&] { ex.spin(run, 20'000'000); });
  rclcpp::shutdown();

  EXPECT_NE(why.find("one stage declares one thread"), std::string::npos) << why;
}
