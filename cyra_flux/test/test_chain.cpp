#include "cyra/flux.hpp"

#include <flux/partitioned_executor.hpp>
#include <rclcpp/rclcpp.hpp>

#include <std_msgs/msg/u_int64.hpp>

#include <gtest/gtest.h>
#include <sched.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

// The chain declaration file reaching real threads. test_spec.cpp (cyra_core) stops at
// the parse: it proves the file says 10/20/30, not that any thread ends up running at those
// values. That gap is the whole point of the file -- a declaration nothing enforces is a comment
// -- and it is where `chrt`, systemd or a later apply by user code would silently win. So the
// assertions here read the policy back from inside the callback, on the thread that runs it.

namespace
{

// A file per test, named by pid so concurrent runs do not share one.
class SpecFile
{
public:
  explicit SpecFile(const std::string & body)
  : path_(
      "/tmp/cyra_chain_" + std::to_string(::getpid()) + "_" + std::to_string(++counter_) + ".yaml")
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

// soft, so a development host's warnings do not block the apply -- what is under test here is
// that the declared values arrive, not whether this host is prepared for hard RT. No cpus: a
// fixed core id names a different core on every machine, and pinning is already covered by
// Schedule.ScheduleReachesTheChildThread.
constexpr const char * kChain = R"(
chains:
  perception_to_control:
    target: soft
    stages:
      - node: /rtchain_camera
        group: capture
        policy: fifo
        priority: 10
      - node: /rtchain_perception
        group: infer
        policy: fifo
        priority: 20
      - node: /rtchain_control
        group: loop
        policy: fifo
        priority: 30
)";

struct Stage
{
  std::shared_ptr<rclcpp::Node> node;
  rclcpp::CallbackGroup::SharedPtr group;
  rclcpp::Subscription<std_msgs::msg::UInt64>::SharedPtr sub;
  std::string label;
  std::atomic<bool> seen{false};
  cyra::ThreadState observed;
};

}  // namespace

// One publish wakes all three stages; each records what the kernel says about the thread its
// callback is running on. Holds on both sides of the permission branch, like every other RT test
// -- Rt.PrivilegedPathIsExercisedWhereDeclared (cyra_core) is what refuses a run that only ever
// took the refusal side.
TEST(Chain, DeclaredStagesReachTheirThreads)
{
  SpecFile file(kChain);
  ::setenv("CYRA_SPEC", file.path().c_str(), 1);
  const auto spec = cyra::Spec::load(file.path());
  ASSERT_EQ(spec.stages().size(), 3u);

  rclcpp::init(0, nullptr);
  auto pub_node = std::make_shared<rclcpp::Node>("rtchain_pub");
  auto pub = pub_node->create_publisher<std_msgs::msg::UInt64>("/rtchain/tick", 10);

  Stage stages[3];
  stages[0].node = std::make_shared<rclcpp::Node>("rtchain_camera");
  stages[0].label = "capture";
  stages[1].node = std::make_shared<rclcpp::Node>("rtchain_perception");
  stages[1].label = "infer";
  stages[2].node = std::make_shared<rclcpp::Node>("rtchain_control");
  stages[2].label = "loop";

  flux::PartitionedExecutor ex;
  for (Stage & s : stages) {
    s.group = s.node->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
    rclcpp::SubscriptionOptions opts;
    opts.callback_group = s.group;
    s.sub = s.node->create_subscription<std_msgs::msg::UInt64>(
      "/rtchain/tick", 10,
      [&s](const std_msgs::msg::UInt64 &) {
        if (!s.seen.exchange(true)) s.observed = cyra::current();
      },
      opts);
    ex.add_node(s.node);
    // The call under test: the file names the stage, and nothing about its scheduling is here.
    cyra::flux::bind(ex, *s.node, s.group, s.label);
  }

  const cyra::Report rep = cyra::preflight(spec.stages().front().opts);
  const cyra::Finding * perm = rep.find("policy-permission");
  ASSERT_NE(perm, nullptr);

  if (perm->verdict != cyra::Verdict::Ok) {
    // The declaration must fail loudly rather than leave the chain running at default priority.
    EXPECT_THROW(ex.spin(), std::runtime_error);
    rclcpp::shutdown();
    ::unsetenv("CYRA_SPEC");
    return;
  }

  std::thread spinner([&] { ex.spin(); });
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  std_msgs::msg::UInt64 m;
  while (std::chrono::steady_clock::now() < deadline) {
    if (stages[0].seen.load() && stages[1].seen.load() && stages[2].seen.load()) break;
    pub->publish(m);
    std::this_thread::sleep_for(2ms);
  }
  ex.cancel();
  spinner.join();
  rclcpp::shutdown();
  ::unsetenv("CYRA_SPEC");

  const int declared[3] = {10, 20, 30};
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(stages[i].seen.load()) << "stage " << i << " never received a message";
    EXPECT_EQ(stages[i].observed.policy, SCHED_FIFO)
      << "stage " << i << " declared fifo but its thread runs policy " << stages[i].observed.policy;
    EXPECT_EQ(stages[i].observed.priority, declared[i])
      << "stage " << i << " declared priority " << declared[i] << " but its thread runs at "
      << stages[i].observed.priority;
  }
  // The rule the file exists to enforce, checked on the running threads rather than on the text
  // that asked for it: priority climbs along the flow, so no stage can preempt the one it feeds.
  EXPECT_LT(stages[0].observed.priority, stages[1].observed.priority);
  EXPECT_LT(stages[1].observed.priority, stages[2].observed.priority);
}

// A node's own worker thread is a chain stage too: no executor owns it, so it applies its own
// stage. The label is whatever the file calls it -- nothing requires a stage to name an rclcpp
// callback group, and a thread the node spawned is exactly the case that has none.
TEST(Chain, AWorkerThreadAppliesItsOwnStage)
{
  SpecFile file(R"(
chains:
  perception_to_control:
    target: soft
    stages:
      - node: /rtchain_worker
        group: infer_worker
        policy: fifo
        priority: 20
      - node: /rtchain_ctl
        group: loop
        policy: fifo
        priority: 30
)");
  const auto spec = cyra::Spec::load(file.path());
  const cyra::Stage * st = spec.find("/rtchain_worker", "infer_worker");
  ASSERT_NE(st, nullptr);
  // A value a node cannot derive from its own line, carried by the stage rather than by the call
  // site -- which is why apply_checked takes the stage whole.
  EXPECT_EQ(st->strict, cyra::Strictness::Soft);

  const cyra::Report rep = cyra::preflight(st->opts);
  const cyra::Finding * perm = rep.find("policy-permission");
  ASSERT_NE(perm, nullptr);

  cyra::ThreadState observed;
  bool threw = false;
  std::thread worker([&] {
    try {
      cyra::apply_checked(*st);
      observed = cyra::current();
    } catch (const std::runtime_error &) {
      threw = true;
    }
  });
  worker.join();

  if (perm->verdict != cyra::Verdict::Ok) {
    EXPECT_TRUE(threw) << "an unprivileged apply must refuse, not leave the thread at default";
    return;
  }
  EXPECT_FALSE(threw);
  EXPECT_EQ(observed.policy, SCHED_FIFO);
  EXPECT_EQ(observed.priority, 20) << "the worker does not run at the priority the file declared";
}

// The check that outlives the apply. apply_checked reads the thread back at the moment it sets
// it, which says nothing about the next second: `chrt`, systemd or a later apply by user code can
// move the thread, and the declaration would go on being a comment. verify() reads the thread
// again on demand, so the move is visible.
//
// Deliberately not an RT stage. Affinity needs no privilege, so this holds the same on a host
// where nothing may run SCHED_FIFO -- the drift and its detection are the same mechanism either
// way, and a test that only ran where RT is permitted would be one more vacuous green.
TEST(Chain, VerifySeesADriftAwayFromTheDeclaration)
{
  const auto allowed = cyra::current().cpus;
  ASSERT_FALSE(allowed.empty());
  const std::uint32_t pinned = allowed.front();

  SpecFile file(
    "chains:\n  drift:\n    target: soft\n    stages:\n      - node: /rtchain_drift\n"
    "        group: worker\n        policy: other\n        cpus: [" +
    std::to_string(pinned) + "]\n");
  const auto spec = cyra::Spec::load(file.path());
  const cyra::Stage * st = spec.find("/rtchain_drift", "worker");
  ASSERT_NE(st, nullptr);

  std::atomic<int> tid{0};
  std::atomic<bool> ready{false};
  std::atomic<bool> stop{false};
  std::thread worker([&] {
    cyra::apply_checked(*st);
    tid.store(cyra::this_tid());
    ready.store(true);
    while (!stop.load()) std::this_thread::sleep_for(1ms);
  });
  while (!ready.load()) std::this_thread::sleep_for(1ms);

  const cyra::Report before = cyra::verify(*st, tid.load());
  EXPECT_TRUE(before.ok()) << before.to_string();
  ASSERT_NE(before.find("observed-cpus"), nullptr);
  EXPECT_EQ(before.find("observed-cpus")->verdict, cyra::Verdict::Ok);

  // Somebody else moves the thread, which is the whole failure mode: no cyra call is involved.
  // SCHED_BATCH because the move must be possible everywhere this test runs -- it needs neither
  // RT privilege nor a second cpu to drift to, so there is no host on which this reduces to a
  // skip.
  sched_param zero{};
  ASSERT_EQ(::sched_setscheduler(tid.load(), SCHED_BATCH, &zero), 0);

  const cyra::Report after = cyra::verify(*st, tid.load());
  stop.store(true);
  worker.join();

  EXPECT_FALSE(after.ok()) << "the thread left the declared policy and verify called it fine:\n"
                           << after.to_string();
  ASSERT_NE(after.find("observed-policy"), nullptr);
  EXPECT_EQ(after.find("observed-policy")->verdict, cyra::Verdict::Fail);
}

// The affinity claim is observed within declared, the reading apply_checked confirms. A file that
// lists the cpus in another order, or a cpuset that narrows the mask to some of them, has not
// moved the thread off any declared cpu.
//
// Deliberately not an RT stage, for the reason VerifySeesADriftAwayFromTheDeclaration gives.
TEST(Chain, VerifyReadsAffinityAsWithinTheDeclaration)
{
  const auto allowed = cyra::current().cpus;
  if (allowed.size() < 2) GTEST_SKIP() << "cyra-cap:two-cpus needs two cpus to declare in reverse";
  const std::uint32_t lo = allowed[0];
  const std::uint32_t hi = allowed[1];

  SpecFile file(
    "chains:\n  within:\n    target: soft\n    stages:\n      - node: /rtchain_within\n"
    "        group: worker\n        policy: other\n        cpus: [" +
    std::to_string(hi) + ", " + std::to_string(lo) + "]\n");
  const auto spec = cyra::Spec::load(file.path());
  const cyra::Stage * st = spec.find("/rtchain_within", "worker");
  ASSERT_NE(st, nullptr);

  std::atomic<int> tid{0};
  std::atomic<bool> ready{false};
  std::atomic<bool> stop{false};
  std::thread worker([&] {
    cyra::apply_checked(*st);
    tid.store(cyra::this_tid());
    ready.store(true);
    while (!stop.load()) std::this_thread::sleep_for(1ms);
  });
  while (!ready.load()) std::this_thread::sleep_for(1ms);

  const cyra::Report reversed = cyra::verify(*st, tid.load());
  ASSERT_NE(reversed.find("observed-cpus"), nullptr);
  EXPECT_EQ(reversed.find("observed-cpus")->verdict, cyra::Verdict::Ok)
    << "the kernel lists cpus ascending and the file did not:\n"
    << reversed.to_string();

  // Narrowed to one of the declared cpus, as a cpuset would: still within.
  cpu_set_t one;
  CPU_ZERO(&one);
  CPU_SET(static_cast<int>(lo), &one);
  ASSERT_EQ(::sched_setaffinity(tid.load(), sizeof one, &one), 0);
  const cyra::Report narrowed = cyra::verify(*st, tid.load());
  stop.store(true);
  worker.join();
  EXPECT_EQ(narrowed.find("observed-cpus")->verdict, cyra::Verdict::Ok) << narrowed.to_string();
}

// The other side of the same claim: a cpu the file never named is out, however the thread got
// there. Two cpus suffice, so this is not folded into the test above behind a third-cpu branch
// that a two-cpu host would silently skip.
TEST(Chain, VerifyFailsAThreadOnACpuOutsideTheDeclaration)
{
  const auto allowed = cyra::current().cpus;
  if (allowed.size() < 2) GTEST_SKIP() << "cyra-cap:two-cpus needs a cpu outside the declaration";
  const std::uint32_t declared = allowed[0];
  const std::uint32_t outside = allowed[1];

  SpecFile file(
    "chains:\n  outside:\n    target: soft\n    stages:\n      - node: /rtchain_outside\n"
    "        group: worker\n        policy: other\n        cpus: [" +
    std::to_string(declared) + "]\n");
  const auto spec = cyra::Spec::load(file.path());
  const cyra::Stage * st = spec.find("/rtchain_outside", "worker");
  ASSERT_NE(st, nullptr);

  std::atomic<int> tid{0};
  std::atomic<bool> ready{false};
  std::atomic<bool> stop{false};
  std::thread worker([&] {
    cyra::apply_checked(*st);
    tid.store(cyra::this_tid());
    ready.store(true);
    while (!stop.load()) std::this_thread::sleep_for(1ms);
  });
  while (!ready.load()) std::this_thread::sleep_for(1ms);

  cpu_set_t other;
  CPU_ZERO(&other);
  CPU_SET(static_cast<int>(outside), &other);
  ASSERT_EQ(::sched_setaffinity(tid.load(), sizeof other, &other), 0);
  const cyra::Report moved = cyra::verify(*st, tid.load());
  stop.store(true);
  worker.join();

  ASSERT_NE(moved.find("observed-cpus"), nullptr);
  EXPECT_EQ(moved.find("observed-cpus")->verdict, cyra::Verdict::Fail) << moved.to_string();
  EXPECT_FALSE(moved.ok());
}

// The other question the file answers: not "is this stage's thread right" but "is anything running
// RT the file never described". The thread holding a stage is explained by it, and so is a child
// of a stage that declares children=inherit. A thread that merely matches a stage's values got
// there without cyra and is named.
TEST(Chain, VerifyProcessNamesOnlyTheThreadsTheFileCannotExplain)
{
  SpecFile file(R"(
chains:
  scan:
    target: soft
    stages:
      - {node: /rtchain_scan, group: worker, policy: fifo, priority: 50}
      - {node: /rtchain_scan, group: pool, policy: fifo, priority: 60, children: inherit}
)");
  const auto spec = cyra::Spec::load(file.path());
  const cyra::Report perm_rep = cyra::preflight(spec.stage("/rtchain_scan", "pool").opts);
  if (perm_rep.find("policy-permission")->verdict != cyra::Verdict::Ok) {
    GTEST_SKIP() << "cyra-cap:rtprio host does not permit SCHED_FIFO";
  }

  std::atomic<bool> stop{false};
  std::atomic<int> started{0};
  std::atomic<int> holder_tid{0};
  std::atomic<int> copycat_tid{0};
  std::atomic<int> helper_tid{0};
  auto park = [&] {
    ++started;
    while (!stop.load()) std::this_thread::sleep_for(1ms);
  };
  std::thread holder([&] {
    cyra::apply_checked(spec.stage("/rtchain_scan", "worker"));
    holder_tid.store(cyra::this_tid());
    park();
  });
  std::thread copycat([&] {
    sched_param p{};
    p.sched_priority = 50;
    ::sched_setscheduler(0, SCHED_FIFO, &p);
    copycat_tid.store(cyra::this_tid());
    park();
  });
  std::thread pool([&] {
    cyra::apply_checked(spec.stage("/rtchain_scan", "pool"));
    std::thread helper([&] {
      helper_tid.store(cyra::this_tid());
      park();
    });
    park();
    helper.join();
  });
  while (started.load() < 4) std::this_thread::sleep_for(1ms);

  const cyra::Report rep = cyra::verify_process(spec);
  stop.store(true);
  holder.join();
  copycat.join();
  pool.join();

  auto named = [&](int tid) {
    for (const cyra::Finding & f : rep.findings) {
      if (f.detail.find("tid " + std::to_string(tid) + " ") != std::string::npos) return true;
    }
    return false;
  };
  EXPECT_FALSE(named(holder_tid.load())) << "the holder of a stage:\n" << rep.to_string();
  EXPECT_FALSE(named(helper_tid.load())) << "a child of an inherit stage:\n" << rep.to_string();
  EXPECT_TRUE(named(copycat_tid.load()))
    << "a thread at a stage's values that cyra never put there:\n"
    << rep.to_string();
  EXPECT_TRUE(rep.ok()) << "an undeclared thread is reported, not refused";
}

// The only thing cyra ever does for a thread it does not own. The file records what the stage
// should already be running at; without this it records it to nobody.
TEST(Chain, AnExternalStageIsCheckedNotAssumed)
{
  SpecFile file(R"(
chains:
  sensor_in:
    target: soft
    stages:
      - node: /rtchain_foreign
        group: listener
        external: true
        expect_priority: 60
)");
  const auto spec = cyra::Spec::load(file.path());
  const cyra::Stage * st = spec.find("/rtchain_foreign", "listener");
  ASSERT_NE(st, nullptr);

  cyra::Options as_owner_would;
  as_owner_would.policy = cyra::Policy::Fifo;
  as_owner_would.priority = 60;
  // An rmw listener does not set SCHED_RESET_ON_FORK, and the flag would stop the unprivileged
  // move below from clearing it.
  as_owner_would.children = cyra::Children::Inherit;
  const cyra::Report perm_rep = cyra::preflight(as_owner_would);
  const cyra::Finding * perm = perm_rep.find("policy-permission");
  ASSERT_NE(perm, nullptr);
  const bool privileged = perm->verdict == cyra::Verdict::Ok;

  std::atomic<int> tid{0};
  std::atomic<bool> ready{false};
  std::atomic<bool> stop{false};
  std::thread foreign([&] {
    // Stands in for the thread's real owner: an rmw listener, a driver, a CUDA worker. cyra never
    // sets this one, which is what `external` declares.
    if (privileged) cyra::detail::apply(as_owner_would);
    tid.store(cyra::this_tid());
    ready.store(true);
    while (!stop.load()) std::this_thread::sleep_for(1ms);
  });
  while (!ready.load()) std::this_thread::sleep_for(1ms);

  const cyra::Report seen = cyra::verify(*st, tid.load());
  ASSERT_NE(seen.find("observed-priority"), nullptr);

  if (!privileged) {
    // The declaration is simply unmet here, and saying so is the correct answer -- an unmet
    // expectation must not read the same as a met one just because cyra could not have helped.
    EXPECT_FALSE(seen.ok()) << "an external stage nobody put on SCHED_FIFO passed:\n"
                            << seen.to_string();
    stop.store(true);
    foreign.join();
    return;
  }

  EXPECT_TRUE(seen.ok()) << seen.to_string();
  EXPECT_EQ(seen.find("observed-priority")->verdict, cyra::Verdict::Ok);

  sched_param wrong{};
  wrong.sched_priority = 50;
  ASSERT_EQ(::sched_setscheduler(tid.load(), SCHED_FIFO, &wrong), 0);
  const cyra::Report drifted = cyra::verify(*st, tid.load());
  stop.store(true);
  foreign.join();

  EXPECT_FALSE(drifted.ok());
  EXPECT_EQ(drifted.find("observed-priority")->verdict, cyra::Verdict::Fail);
}

// A stage whose thread is not there is the case a declaration exists to catch: it means nothing
// ever applied it, or what did apply it has exited. Reading that as a thread at rest would turn
// the loudest failure into the quietest pass.
TEST(Chain, AStageWhoseThreadIsGoneIsAFailureNotASilence)
{
  SpecFile file(R"(
chains:
  gone:
    target: soft
    stages:
      - node: /rtchain_gone
        group: worker
        policy: fifo
        priority: 20
)");
  const auto spec = cyra::Spec::load(file.path());
  const cyra::Stage * st = spec.find("/rtchain_gone", "worker");
  ASSERT_NE(st, nullptr);

  std::ifstream f("/proc/sys/kernel/pid_max");
  int pid_max = 0;
  ASSERT_TRUE(f >> pid_max);

  const cyra::Report rep = cyra::verify(*st, pid_max + 1);
  EXPECT_FALSE(rep.ok());
  ASSERT_NE(rep.find("observed-thread"), nullptr);
  EXPECT_EQ(rep.find("observed-thread")->verdict, cyra::Verdict::Fail);
}

// An external stage is the file saying "this thread is somebody else's". Applying or scheduling it
// would put that thread under cyra after all, which is the one thing the marking rules out.
TEST(Chain, AnExternalStageIsRefusedByBothEntryPoints)
{
  SpecFile file(R"(
chains:
  sensor_in:
    target: soft
    stages:
      - node: /rtchain_ext
        group: dds_listener
        external: true
        expect_priority: 60
      - node: /rtchain_ext
        group: work
        policy: fifo
        priority: 70
)");
  const auto spec = cyra::Spec::load(file.path());

  rclcpp::init(0, nullptr);
  auto node = std::make_shared<rclcpp::Node>("rtchain_ext");
  auto group = node->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);

  flux::PartitionedExecutor ex;
  ex.add_node(node);
  const cyra::Stage & ext = spec.stage(node->get_fully_qualified_name(), "dds_listener");
  EXPECT_THROW(cyra::flux::apply_checked(ex, *node, group, ext), std::invalid_argument);
  EXPECT_THROW(cyra::apply_checked(ext), std::invalid_argument);
  EXPECT_NO_THROW(cyra::flux::apply_checked(
    ex, *node, group, spec.stage(node->get_fully_qualified_name(), "work")));
  rclcpp::shutdown();
}
