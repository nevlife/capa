#include "cyra/spec.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>

// The chain declaration file. Every rule here exists because a node declaring
// its own priority in isolation cannot check it: the ordering spans processes, and so does the
// conflict between two chains that name the same stage.

namespace
{

// A file per test, named by pid so concurrent runs do not share one.
class SpecFile
{
public:
  explicit SpecFile(const std::string & body)
  : path_(
      "/tmp/cyra_spec_" + std::to_string(::getpid()) + "_" + std::to_string(++counter_) + ".yaml")
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

constexpr const char * kGood = R"(
chains:
  perception_to_control:
    target: hard
    stages:
      - node: /camera_node
        group: dds_listener
        external: true
        expect_priority: 60
      - node: /camera_node
        policy: fifo
        priority: 70
        cpus: [2]
      - node: /perception_node
        group: infer
        policy: fifo
        priority: 75
      - node: /control_node
        group: loop
        policy: fifo
        priority: 90
)";

}  // namespace

TEST(Spec, ParsesAChainAndDerivesTheControlPriority)
{
  SpecFile f(kGood);
  const auto spec = cyra::Spec::load(f.path());
  EXPECT_FALSE(spec.empty());
  EXPECT_EQ(spec.stages().size(), 4u);

  const auto * cam = spec.find("/camera_node");
  ASSERT_NE(cam, nullptr);
  EXPECT_EQ(cam->opts.priority, 70);
  EXPECT_EQ(cam->opts.policy, cyra::Policy::Fifo);
  EXPECT_EQ(cam->opts.cpus, std::vector<std::uint32_t>{2});
  EXPECT_EQ(cam->strict, cyra::Strictness::Hard) << "target: hard must reach every stage";

  // The whole reason the chain lives in one file: no node can derive this from its own line.
  EXPECT_EQ(cam->control_priority, 90) << "upstream stages are checked against the control loop";

  const auto * ctl = spec.find("/control_node", "loop");
  ASSERT_NE(ctl, nullptr);
  EXPECT_EQ(ctl->control_priority, 0) << "the control stage is not checked against itself";

  const auto ext = spec.external();
  ASSERT_EQ(ext.size(), 1u);
  EXPECT_EQ(ext[0]->expect_priority, 60);
  EXPECT_EQ(ext[0]->opts.policy, cyra::Policy::Inherit) << "cyra never sets an external stage";
}

TEST(Spec, RejectsAChainThatDoesNotClimb)
{
  // Perception above control starves the loop it feeds. This is preflight's priority-order rule
  // past two stages, and only the file can see it.
  SpecFile f(R"(
chains:
  inverted:
    target: soft
    stages:
      - node: /camera_node
        policy: fifo
        priority: 70
      - node: /perception_node
        policy: fifo
        priority: 95
      - node: /control_node
        policy: fifo
        priority: 90
)");
  EXPECT_THROW(cyra::Spec::load(f.path()), std::runtime_error);
}

TEST(Spec, NonRtStagesSitOutsideTheOrdering)
{
  // A SCHED_OTHER consumer at the end of a chain is not a priority inversion: it never takes part
  // in RT priority at all. Requiring it to keep climbing would reject a normal logging path.
  SpecFile f(R"(
chains:
  logging:
    target: soft
    stages:
      - node: /camera_node
        policy: fifo
        priority: 70
      - node: /logger_node
        policy: other
)");
  EXPECT_NO_THROW(cyra::Spec::load(f.path()));
}

TEST(Spec, AStageMaySitOnTwoChainsButNotTwoWays)
{
  const char * body = R"(
chains:
  control:
    target: hard
    stages:
      - node: /camera_node
        policy: fifo
        priority: 70
      - node: /control_node
        policy: fifo
        priority: 90
  logging:
    target: soft
    stages:
      - node: /camera_node
        policy: fifo
        priority: %d
      - node: /logger_node
        policy: other
)";
  char same[1024];
  std::snprintf(same, sizeof same, body, 70);
  SpecFile agree(same);
  EXPECT_NO_THROW(cyra::Spec::load(agree.path())) << "one stage feeding two chains is normal";

  char differ[1024];
  std::snprintf(differ, sizeof differ, body, 40);
  SpecFile conflict(differ);
  EXPECT_THROW(cyra::Spec::load(conflict.path()), std::runtime_error)
    << "two chains asking one thread to run differently is the conflict only a shared file sees";
}

// The stage's own declaration is the same on both chains, so it is not a conflict. Which chain the
// file lists first is layout, not meaning: a camera that feeds a hard control chain is a hard
// stage, and a soft logging chain listed above it must not read its Warn findings for it.
TEST(Spec, TheStrictestChainAStageSitsOnDecidesItsStrictness)
{
  SpecFile f(R"(
chains:
  logging:
    target: soft
    stages:
      - node: /camera_node
        policy: fifo
        priority: 70
      - node: /logger_node
        policy: other
  control:
    target: hard
    stages:
      - node: /camera_node
        policy: fifo
        priority: 70
      - node: /control_node
        policy: fifo
        priority: 90
)");
  const auto spec = cyra::Spec::load(f.path());
  const auto * cam = spec.find("/camera_node");
  ASSERT_NE(cam, nullptr);
  EXPECT_EQ(cam->strict, cyra::Strictness::Hard)
    << "the soft chain came first in the file and the camera was read as soft";
  const auto * logger = spec.find("/logger_node");
  ASSERT_NE(logger, nullptr);
  EXPECT_EQ(logger->strict, cyra::Strictness::Soft) << "the logging chain itself stays soft";
}

TEST(Spec, ExternalExpectPriorityIsAnRtPriorityOrUndeclared)
{
  const char * body = R"(
chains:
  c:
    target: soft
    stages:
      - node: /a
        external: true
        expect_priority: %d
)";
  for (int bad : {-1, 100}) {
    char text[512];
    std::snprintf(text, sizeof text, body, bad);
    SpecFile f(text);
    EXPECT_THROW(cyra::Spec::load(f.path()), std::runtime_error)
      << bad << " is not a priority any thread can run at, and verify would only ever fail it";
  }
  for (int fine : {0, 1, 99}) {
    char text[512];
    std::snprintf(text, sizeof text, body, fine);
    SpecFile f(text);
    EXPECT_NO_THROW(cyra::Spec::load(f.path()))
      << fine << " is the range edge or the undeclared value";
  }
}

TEST(Spec, RejectsWhatItDidNotRead)
{
  // No version field: a version number announces a schema change and says nothing about a typo.
  // Refusing every key we did not read catches both.
  SpecFile typo(R"(
chains:
  c:
    target: soft
    stages:
      - node: /a
        policy: fifo
        priorty: 70
)");
  EXPECT_THROW(cyra::Spec::load(typo.path()), std::runtime_error);

  SpecFile stray_target(R"(
chains:
  c:
    target: medium
    stages:
      - node: /a
        policy: other
)");
  EXPECT_THROW(cyra::Spec::load(stray_target.path()), std::runtime_error);

  SpecFile relative(R"(
chains:
  c:
    target: soft
    stages:
      - node: a
        policy: other
)");
  EXPECT_THROW(cyra::Spec::load(relative.path()), std::runtime_error)
    << "a node name that is not fully qualified names a different node than the running one";
}

TEST(Spec, ExternalStagesCarryNoThreadOptions)
{
  // An external stage is a record of somebody else's thread. Options here would read as something
  // cyra applies, and it never does.
  SpecFile f(R"(
chains:
  c:
    target: soft
    stages:
      - node: /a
        external: true
        policy: fifo
        priority: 60
)");
  EXPECT_THROW(cyra::Spec::load(f.path()), std::runtime_error);
}

TEST(Spec, LookupByNodeRejectsAnUnknownLabel)
{
  SpecFile f(kGood);
  const auto spec = cyra::Spec::load(f.path());

  EXPECT_NO_THROW(spec.stage("/perception_node", "infer"));
  EXPECT_EQ(spec.stage("/perception_node", "infer").opts.priority, 75);

  // Silently applying nothing is how a typo survives to runtime.
  EXPECT_THROW(spec.stage("/perception_node", "infr"), std::runtime_error);
  EXPECT_THROW(spec.stage("/not_in_the_spec"), std::runtime_error);
}

TEST(Spec, NoEnvIsAnEmptySpec)
{
  ::unsetenv("CYRA_SPEC");
  const auto spec = cyra::Spec::load();
  EXPECT_TRUE(spec.empty()) << "declaring nothing stays the no-op it is today";
}

TEST(Spec, HardChainsGiveEachRtThreadItsOwnCore)
{
  // Splitting callbacks into groups is how they stop blocking each other; pinning two of those
  // groups to one core undoes it. SCHED_FIFO has no timeslice, so the first one there holds the
  // core until it blocks. It also breaks nohz_full, which needs at most one runnable task.
  SpecFile hard(R"(
chains:
  c:
    target: hard
    stages:
      - node: /a
        group: x
        policy: fifo
        priority: 70
        cpus: [2]
      - node: /b
        group: y
        policy: fifo
        priority: 80
        cpus: [2]
)");
  EXPECT_THROW(cyra::Spec::load(hard.path()), std::runtime_error);

  // Different cores is the arrangement the split was for.
  SpecFile apart(R"(
chains:
  c:
    target: hard
    stages:
      - node: /a
        group: x
        policy: fifo
        priority: 70
        cpus: [2]
      - node: /b
        group: y
        policy: fifo
        priority: 80
        cpus: [3]
)");
  EXPECT_NO_THROW(cyra::Spec::load(apart.path()));
}

TEST(Spec, SoftChainsMayShareAnRtCore)
{
  // Two RT threads on one core at different priorities is ordinary Linux, and the file cannot
  // tell whether the core is isolated at all. Strictness draws the line, as it does over
  // preflight's Warn findings.
  SpecFile f(R"(
chains:
  c:
    target: soft
    stages:
      - node: /a
        group: x
        policy: fifo
        priority: 70
        cpus: [2]
      - node: /b
        group: y
        policy: fifo
        priority: 80
        cpus: [2]
)");
  EXPECT_NO_THROW(cyra::Spec::load(f.path()));
}

TEST(Spec, TheCollisionIsFoundAcrossChainsToo)
{
  // Two chains can put two stages on one core without either chain being wrong on its own. Only
  // a file holding both sees it -- the same reason the conflicting-declaration check exists.
  SpecFile f(R"(
chains:
  control:
    target: hard
    stages:
      - node: /a
        group: x
        policy: fifo
        priority: 70
        cpus: [2]
  vision:
    target: hard
    stages:
      - node: /b
        group: y
        policy: fifo
        priority: 80
        cpus: [2]
)");
  EXPECT_THROW(cyra::Spec::load(f.path()), std::runtime_error);
}

TEST(Spec, NonRtStagesDoNotCollide)
{
  // SCHED_OTHER threads sharing a core is what every Linux box does. The rule is about RT
  // threads blocking each other, not about co-tenancy.
  SpecFile f(R"(
chains:
  c:
    target: hard
    stages:
      - node: /a
        group: x
        policy: other
        cpus: [2]
      - node: /b
        group: y
        policy: other
        cpus: [2]
)");
  EXPECT_NO_THROW(cyra::Spec::load(f.path()));
}

// One stage names one thread. A second live thread taking it would leave the file describing
// neither; once the holder exits, a new thread may take it (an executor spun again).
TEST(Spec, OneStageIsHeldByOneLiveThread)
{
  SpecFile f(R"(
chains:
  c:
    target: soft
    stages:
      - node: /claim
        group: worker
        cpus: [0]
)");
  const auto spec = cyra::Spec::load(f.path());
  const cyra::Stage & stage = spec.stage("/claim", "worker");

  std::promise<void> applied;
  std::promise<void> release;
  std::thread holder([&] {
    cyra::apply_checked(stage);
    cyra::apply_checked(stage);  // the holder re-applying is not a second thread
    applied.set_value();
    release.get_future().wait();
  });
  applied.get_future().wait();

  std::string why = "<no exception>";
  std::thread second([&] {
    try {
      cyra::apply_checked(stage);
    } catch (const std::invalid_argument & e) {
      why = e.what();
    }
  });
  second.join();
  release.set_value();
  holder.join();
  EXPECT_NE(why.find("one stage declares one thread"), std::string::npos) << why;

  std::thread after([&] { EXPECT_NO_THROW(cyra::apply_checked(stage)); });
  after.join();
}
