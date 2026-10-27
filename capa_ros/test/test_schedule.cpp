#include "capa/flux.hpp"

#include <flux/partitioned_executor.hpp>
#include <rclcpp/rclcpp.hpp>

#include <std_msgs/msg/u_int64.hpp>

#include <gtest/gtest.h>
#include <sched.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

// capa::flux::apply_checked(): the child thread serving the group applies the declared scheduling
// to itself before its first callback. Affinity needs no privilege, so it proves end to end that
// the options reach the child thread: the callback reads its own affinity back from the kernel.
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
      if (!seen.exchange(true)) cpus_in_callback = capa::current().cpus;
    },
    opts);

  flux::PartitionedExecutor ex;
  ex.add_node(sub_node);
  capa::Options sched;
  sched.cpus = {0};
  capa::flux::apply_checked(ex, *sub_node, g, sched);

  std::thread spinner([&] { ex.spin(); });

  const auto deadline = std::chrono::steady_clock::now() + 5s;
  std_msgs::msg::UInt64 m;
  while (!seen.load() && std::chrono::steady_clock::now() < deadline) {
    ros_pub->publish(m);
    std::this_thread::sleep_for(2ms);
  }

  ex.cancel();
  spinner.join();
  rclcpp::shutdown();

  ASSERT_TRUE(seen.load()) << "the scheduled group never received a message";
  EXPECT_EQ(cpus_in_callback, (std::vector<std::uint32_t>{0}))
    << "the callback did not run on the pinned cpu set";
}

// An RT-policy schedule follows the capa::apply contract through the executor: where preflight
// says the policy is permitted the child thread must actually hold it, and where it is not the
// refusal must surface as the spin() error -- never as a thread silently left at the default.
TEST(Schedule, ScheduleFifoMatchesPreflight)
{
  capa::Options sched;
  sched.policy = capa::Policy::Fifo;
  sched.priority = 1;
  const capa::Report rep = capa::preflight(sched);
  const capa::Finding * perm = rep.find("policy-permission");
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
      if (!seen.exchange(true)) policy_in_callback = capa::current().policy;
    },
    opts);

  flux::PartitionedExecutor ex;
  ex.add_node(sub_node);
  capa::flux::apply_checked(ex, *sub_node, g, sched);

  if (perm->verdict != capa::Verdict::Ok) {
    // std::runtime_error, not system_error: the child goes through capa::apply_checked, so the
    // refusal now comes from preflight before the thread is touched rather than from the
    // syscall afterwards. system_error derives from runtime_error, so this covers both.
    EXPECT_THROW(ex.spin(), std::runtime_error);
    rclcpp::shutdown();
    return;
  }

  std::thread spinner([&] { ex.spin(); });
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  std_msgs::msg::UInt64 m;
  while (!seen.load() && std::chrono::steady_clock::now() < deadline) {
    ros_pub->publish(m);
    std::this_thread::sleep_for(2ms);
  }
  ex.cancel();
  spinner.join();
  rclcpp::shutdown();

  ASSERT_TRUE(seen.load()) << "the scheduled group never received a message";
  EXPECT_EQ(policy_in_callback, SCHED_FIFO) << "preflight said Ok but the child is not FIFO";
}

// The surface rejects everything it cannot honor: a group of another node, options validate()
// refuses, a second declaration for the same group, a declaration for a group no child will
// serve, and any declaration once spinning. A null group is the node's default one.
TEST(Schedule, ScheduleRejects)
{
  rclcpp::init(0, nullptr);
  auto node = std::make_shared<rclcpp::Node>("cie_sched_reject");
  auto other = std::make_shared<rclcpp::Node>("cie_sched_other");

  flux::PartitionedExecutor ex;
  ex.add_node(node);

  auto foreign = other->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
  EXPECT_THROW(
    capa::flux::apply_checked(ex, *node, foreign, capa::Options{}), std::invalid_argument);

  auto g = node->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
  capa::Options bad;
  bad.policy = capa::Policy::Fifo;  // priority 0 with an RT policy: reinterpretable
  EXPECT_THROW(capa::flux::apply_checked(ex, *node, g, bad), std::invalid_argument);

  capa::flux::apply_checked(ex, *node, g, capa::Options{});
  EXPECT_THROW(
    capa::flux::apply_checked(ex, *node, g, capa::Options{}),
    std::invalid_argument);  // one declaration per group
  EXPECT_NO_THROW(capa::flux::apply_checked(ex, *node, nullptr, capa::Options{}));
  EXPECT_THROW(
    capa::flux::apply_checked(
      ex, *node, node->get_node_base_interface()->get_default_callback_group(), capa::Options{}),
    std::invalid_argument)
    << "nullptr and the default group are the same group";

  // A group of a node never handed over: no child will serve it, so the declaration would
  // silently never apply. spin() refuses to start.
  auto stray_node = std::make_shared<rclcpp::Node>("cie_sched_stray");
  auto stray = stray_node->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
  capa::flux::apply_checked(ex, *stray_node, stray, capa::Options{});
  EXPECT_THROW(ex.spin(), std::invalid_argument);

  flux::PartitionedExecutor ex2;
  ex2.add_node(node);
  std::thread spinner([&] { ex2.spin(); });
  std::this_thread::sleep_for(100ms);
  EXPECT_THROW(capa::flux::apply_checked(ex2, *node, g, capa::Options{}), std::logic_error);
  ex2.cancel();
  spinner.join();
  rclcpp::shutdown();
}

// The child applies through capa::apply_checked, so a host that cannot hold a deadline is refused
// when the caller declared Hard rather than surfacing as a thread that quietly runs non-RT.
TEST(Schedule, HardStrictnessSurfacesAsASpinError)
{
  rclcpp::init(0, nullptr);
  auto node = std::make_shared<rclcpp::Node>("cie_strict");

  capa::Options opts;
  opts.cpus = {0};
  const auto rep = capa::preflight(opts);
  bool warned = false;
  for (const auto & f : rep.findings) {
    if (f.verdict == capa::Verdict::Warn) warned = true;
  }
  if (!rep.ok() || !warned) {
    rclcpp::shutdown();
    GTEST_SKIP() << "capa-cap:rt-warn-verdict this host has no Warn-only verdict to strengthen";
  }

  flux::PartitionedExecutor ex;
  ex.add_node(node);
  auto g = node->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
  capa::flux::apply_checked(ex, *node, g, opts, capa::Strictness::Hard);

  EXPECT_THROW(ex.spin(), std::runtime_error);

  rclcpp::shutdown();
}

// ---- the declaration file's name for a thread, checked against the group handed over ----
//
// A stage names its thread (node, label); an rclcpp callback group has no name at all. The only
// thing linking the two is the pairing at the apply_checked() call.

namespace
{

class SpecFile
{
public:
  explicit SpecFile(const std::string & body)
  : path_(
      "/tmp/capa_ros_spec_" + std::to_string(::getpid()) + "_" + std::to_string(++counter_) +
      ".yaml")
  {
    std::ofstream out(path_);
    out << body;
  }
  SpecFile(const SpecFile &) = delete;
  SpecFile & operator=(const SpecFile &) = delete;
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
    std::forward<F>(call)();
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
  const auto spec = capa::Spec::load(f.path());

  auto g = node->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
  flux::PartitionedExecutor ex;
  const std::string why =
    refusal([&] { capa::flux::apply_checked(ex, *node, g, *spec.find("/cie_label_other")); });
  rclcpp::shutdown();

  EXPECT_NE(why.find("but was handed node"), std::string::npos) << why;
}

TEST(ScheduleStage, RejectsAGroupOfAnotherNode)
{
  rclcpp::init(0, nullptr);
  auto node = std::make_shared<rclcpp::Node>("cie_label");
  auto other = std::make_shared<rclcpp::Node>("cie_label_other");
  SpecFile f(kLabelSpec);
  const auto spec = capa::Spec::load(f.path());

  auto g = other->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
  flux::PartitionedExecutor ex;
  const std::string why =
    refusal([&] { capa::flux::apply_checked(ex, *node, g, *spec.find("/cie_label", "infer")); });
  rclcpp::shutdown();

  EXPECT_NE(why.find("does not belong to node"), std::string::npos) << why;
}

TEST(ScheduleStage, RejectsALabelledStageOnTheDefaultGroup)
{
  rclcpp::init(0, nullptr);
  auto node = std::make_shared<rclcpp::Node>("cie_label");
  SpecFile f(kLabelSpec);
  const auto spec = capa::Spec::load(f.path());

  flux::PartitionedExecutor ex;
  const std::string why = refusal([&] {
    capa::flux::apply_checked(
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
  const auto spec = capa::Spec::load(f.path());

  auto g = node->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
  flux::PartitionedExecutor ex;
  const std::string why =
    refusal([&] { capa::flux::apply_checked(ex, *node, g, *spec.find("/cie_label_other")); });
  rclcpp::shutdown();

  EXPECT_NE(why.find("names no group"), std::string::npos) << why;
}

TEST(ScheduleStage, AcceptsAStageOnTheGroupItNames)
{
  rclcpp::init(0, nullptr);
  auto node = std::make_shared<rclcpp::Node>("cie_label");
  SpecFile f(kLabelSpec);
  const auto spec = capa::Spec::load(f.path());

  auto g = node->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
  flux::PartitionedExecutor ex;
  ex.add_node(node);
  capa::flux::apply_checked(ex, *node, g, *spec.find("/cie_label", "infer"));

  std::thread spinner([&] { ex.spin(); });
  std::this_thread::sleep_for(100ms);
  ex.cancel();
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
  const auto spec = capa::Spec::load(f.path());

  auto ga = node->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
  auto gb = node->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
  flux::PartitionedExecutor ex;
  ex.add_node(node);
  const auto & stage = *spec.find("/cie_label", "infer");
  capa::flux::apply_checked(ex, *node, ga, stage);
  capa::flux::apply_checked(ex, *node, gb, stage);

  const std::string why = refusal([&] { ex.spin(); });
  rclcpp::shutdown();

  EXPECT_NE(why.find("one stage declares one thread"), std::string::npos) << why;
}

namespace
{

// motion_velocity_planner in miniature: the work is in a group the code never names, picked out
// by the subscription it holds.
constexpr const char * kHoldsSpec = R"(
chains:
  c:
    target: soft
    stages:
      - node: /cie_holds
        cpus: [FIRST]
      - node: /cie_holds
        group: traj
        holds: {subscription: /cie_holds/traj}
        cpus: [LAST]
)";

std::string holds_spec(std::uint32_t first, std::uint32_t last)
{
  std::string s = kHoldsSpec;
  s.replace(s.find("FIRST"), 5, std::to_string(first));
  s.replace(s.find("LAST"), 4, std::to_string(last));
  return s;
}

struct Holder
{
  rclcpp::Node::SharedPtr node;
  rclcpp::CallbackGroup::SharedPtr other;
  rclcpp::CallbackGroup::SharedPtr traj;
  std::vector<rclcpp::SubscriptionBase::SharedPtr> subs;
  std::atomic<bool> seen_default{false};
  std::atomic<bool> seen_traj{false};
  std::vector<std::uint32_t> default_cpus;
  std::vector<std::uint32_t> traj_cpus;

  Holder()
  : node(std::make_shared<rclcpp::Node>("cie_holds")),
    other(node->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive)),
    traj(node->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive))
  {
    subscribe("/cie_holds/odom", nullptr, seen_default, default_cpus);
    subs.push_back(node->create_subscription<std_msgs::msg::UInt64>(
      "/cie_holds/map", 10, [](const std_msgs::msg::UInt64 &) {}, options(other)));
    subscribe("/cie_holds/traj", traj, seen_traj, traj_cpus);
  }

  static rclcpp::SubscriptionOptions options(const rclcpp::CallbackGroup::SharedPtr & g)
  {
    rclcpp::SubscriptionOptions o;
    o.callback_group = g;
    return o;
  }

  void subscribe(
    const std::string & topic, const rclcpp::CallbackGroup::SharedPtr & g, std::atomic<bool> & seen,
    std::vector<std::uint32_t> & cpus)
  {
    subs.push_back(node->create_subscription<std_msgs::msg::UInt64>(
      topic, 10,
      [&seen, &cpus](const std_msgs::msg::UInt64 &) {
        if (!seen.exchange(true)) cpus = capa::current().cpus;
      },
      options(g)));
  }
};

}  // namespace

TEST(Container, DeclaredFindsTheGroupByWhatItHolds)
{
  rclcpp::init(0, nullptr);
  {
    Holder h;
    auto stranger = std::make_shared<rclcpp::Node>("cie_holds_other");
    SpecFile f(holds_spec(0, 0));
    const auto spec = capa::Spec::load(f.path());
    auto & base = *h.node->get_node_base_interface();

    const capa::Stage * traj = capa::flux::declared(spec, base, h.traj);
    ASSERT_NE(traj, nullptr);
    EXPECT_EQ(traj->group, "traj");
    EXPECT_EQ(capa::flux::declared(spec, base, h.other), nullptr);
    const capa::Stage * dflt = capa::flux::declared(spec, base, nullptr);
    ASSERT_NE(dflt, nullptr);
    EXPECT_EQ(dflt->group, "");

    const auto g = stranger->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
    EXPECT_THROW(capa::flux::declared(spec, base, g), std::invalid_argument);

    flux::PartitionedExecutor ex;
    const std::string why = refusal(
      [&] { capa::flux::apply_checked(ex, *h.node, h.other, *spec.find("/cie_holds", "traj")); });
    EXPECT_NE(why.find("does not"), std::string::npos) << why;
  }
  rclcpp::shutdown();
}

// A flux subscription is no rclcpp entity; it is in the group through the waitable that
// add_to_callback_group() puts there, and content() reads its topic from that.
TEST(Container, ContentListsAFluxSubscriptionInTheGroup)
{
  rclcpp::init(0, nullptr);
  {
    auto node = std::make_shared<rclcpp::Node>("cie_flux_holds");
    auto g = node->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
    flux::Subscription sub(
      *node, "frames", flux::kNoSchema, flux::QoS{}, [](const flux::FrameView &) {});
    sub.add_to_callback_group(*node, g);
    auto & base = *node->get_node_base_interface();
    EXPECT_EQ(capa::flux::content(base, g).subscriptions, std::vector<std::string>{"/frames"});
    const auto in_default = capa::flux::content(base, nullptr).subscriptions;
    EXPECT_EQ(std::count(in_default.begin(), in_default.end(), "/frames"), 0);
  }
  rclcpp::shutdown();
}

TEST(Container, BindDeclaredReachesTheGroupThatHoldsTheSubscription)
{
  const auto allowed = capa::current().cpus;
  if (allowed.size() < 2) GTEST_SKIP() << "needs two cpus to tell the groups apart";
  SpecFile f(holds_spec(allowed.front(), allowed.back()));
  ::setenv("CAPA_SPEC", f.path().c_str(), 1);
  rclcpp::init(0, nullptr);
  {
    Holder h;
    auto pub_node = std::make_shared<rclcpp::Node>("cie_holds_pub");
    auto odom = pub_node->create_publisher<std_msgs::msg::UInt64>("/cie_holds/odom", 10);
    auto traj = pub_node->create_publisher<std_msgs::msg::UInt64>("/cie_holds/traj", 10);

    flux::PartitionedExecutor ex;
    ex.add_node(h.node);
    capa::flux::bind_declared(ex, *h.node->get_node_base_interface());

    std::thread spinner([&] { ex.spin(); });
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while ((!h.seen_default || !h.seen_traj) && std::chrono::steady_clock::now() < deadline) {
      odom->publish(std_msgs::msg::UInt64{});
      traj->publish(std_msgs::msg::UInt64{});
      std::this_thread::sleep_for(2ms);
    }
    ex.cancel();
    spinner.join();
    ASSERT_TRUE(h.seen_default && h.seen_traj);
    EXPECT_EQ(h.default_cpus, std::vector<std::uint32_t>{allowed.front()});
    EXPECT_EQ(h.traj_cpus, std::vector<std::uint32_t>{allowed.back()});
  }
  rclcpp::shutdown();
  ::unsetenv("CAPA_SPEC");
}
