#include "capa/spec.hpp"

#include <gtest/gtest.h>
#include <pthread.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

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
      "/tmp/capa_spec_" + std::to_string(::getpid()) + "_" + std::to_string(++counter_) + ".yaml")
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
  const auto spec = capa::Spec::load(f.path());
  EXPECT_FALSE(spec.empty());
  EXPECT_EQ(spec.stages().size(), 4u);

  const auto * cam = spec.find("/camera_node");
  ASSERT_NE(cam, nullptr);
  EXPECT_EQ(cam->opts.priority, 70);
  EXPECT_EQ(cam->opts.policy, capa::Policy::Fifo);
  EXPECT_EQ(cam->opts.cpus, std::vector<std::uint32_t>{2});
  EXPECT_EQ(cam->strict, capa::Strictness::Hard) << "target: hard must reach every stage";
  EXPECT_EQ(cam->opts.children, capa::Children::Reset) << "RT does not leak unless declared";
  EXPECT_EQ(cam->kind, capa::Kind::Worker);

  const auto ext = spec.external();
  ASSERT_EQ(ext.size(), 1u);
  EXPECT_EQ(ext[0]->expect_priority, 60);
  EXPECT_EQ(ext[0]->opts.policy, capa::Policy::Inherit) << "capa never sets an external stage";
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
  EXPECT_THROW(capa::Spec::load(f.path()), std::runtime_error);
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
  EXPECT_NO_THROW(capa::Spec::load(f.path()));
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
  EXPECT_NO_THROW(capa::Spec::load(agree.path())) << "one stage feeding two chains is normal";

  char differ[1024];
  std::snprintf(differ, sizeof differ, body, 40);
  SpecFile conflict(differ);
  EXPECT_THROW(capa::Spec::load(conflict.path()), std::runtime_error)
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
  const auto spec = capa::Spec::load(f.path());
  const auto * cam = spec.find("/camera_node");
  ASSERT_NE(cam, nullptr);
  EXPECT_EQ(cam->strict, capa::Strictness::Hard)
    << "the soft chain came first in the file and the camera was read as soft";
  const auto * logger = spec.find("/logger_node");
  ASSERT_NE(logger, nullptr);
  EXPECT_EQ(logger->strict, capa::Strictness::Soft) << "the logging chain itself stays soft";
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
    EXPECT_THROW(capa::Spec::load(f.path()), std::runtime_error)
      << bad << " is not a priority any thread can run at, and verify would only ever fail it";
  }
  for (int fine : {0, 1, 99}) {
    char text[512];
    std::snprintf(text, sizeof text, body, fine);
    SpecFile f(text);
    EXPECT_NO_THROW(capa::Spec::load(f.path()))
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
  EXPECT_THROW(capa::Spec::load(typo.path()), std::runtime_error);

  SpecFile stray_target(R"(
chains:
  c:
    target: medium
    stages:
      - node: /a
        policy: other
)");
  EXPECT_THROW(capa::Spec::load(stray_target.path()), std::runtime_error);

  SpecFile relative(R"(
chains:
  c:
    target: soft
    stages:
      - node: a
        policy: other
)");
  EXPECT_THROW(capa::Spec::load(relative.path()), std::runtime_error)
    << "a node name that is not fully qualified names a different node than the running one";
}

TEST(Spec, ExternalStagesCarryNoThreadOptions)
{
  // An external stage is a record of somebody else's thread. Options here would read as something
  // capa applies, and it never does.
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
  EXPECT_THROW(capa::Spec::load(f.path()), std::runtime_error);
}

TEST(Spec, LookupByNodeRejectsAnUnknownLabel)
{
  SpecFile f(kGood);
  const auto spec = capa::Spec::load(f.path());

  EXPECT_NO_THROW(spec.stage("/perception_node", "infer"));
  EXPECT_EQ(spec.stage("/perception_node", "infer").opts.priority, 75);

  // Silently applying nothing is how a typo survives to runtime.
  EXPECT_THROW(spec.stage("/perception_node", "infr"), std::runtime_error);
  EXPECT_THROW(spec.stage("/not_in_the_spec"), std::runtime_error);
}

TEST(Spec, NoEnvIsAnEmptySpec)
{
  ::unsetenv("CAPA_SPEC");
  const auto spec = capa::Spec::load();
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
  EXPECT_THROW(capa::Spec::load(hard.path()), std::runtime_error);

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
  EXPECT_NO_THROW(capa::Spec::load(apart.path()));
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
  EXPECT_NO_THROW(capa::Spec::load(f.path()));
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
  EXPECT_THROW(capa::Spec::load(f.path()), std::runtime_error);
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
  EXPECT_NO_THROW(capa::Spec::load(f.path()));
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
  const auto spec = capa::Spec::load(f.path());
  const capa::Stage & stage = spec.stage("/claim", "worker");

  std::promise<void> applied;
  std::promise<void> release;
  std::thread holder([&] {
    capa::apply_checked(stage);
    capa::apply_checked(stage);  // the holder re-applying is not a second thread
    applied.set_value();
    release.get_future().wait();
  });
  applied.get_future().wait();

  std::string why = "<no exception>";
  std::thread second([&] {
    try {
      capa::apply_checked(stage);
    } catch (const std::invalid_argument & e) {
      why = e.what();
    }
  });
  second.join();
  release.set_value();
  holder.join();
  EXPECT_NE(why.find("one stage declares one thread"), std::string::npos) << why;

  std::thread after([&] { EXPECT_NO_THROW(capa::apply_checked(stage)); });
  after.join();
}

// ---- v2 rules: where two stages can meet on a cpu, and what kind of thread each one is ----

namespace
{

std::string load_error(const std::string & body)
{
  SpecFile f(body);
  try {
    capa::Spec::load(f.path());
  } catch (const std::runtime_error & e) {
    return e.what();
  }
  return "<loaded>";
}

}  // namespace

TEST(Spec, TheFlowRuleBindsOnlyStagesThatShareACpu)
{
  // Disjoint cpus: neither can preempt the other, so upstream may run higher.
  EXPECT_EQ(
    load_error(R"(
chains:
  c:
    target: soft
    stages:
      - {node: /a, policy: fifo, priority: 80, cpus: [2]}
      - {node: /b, policy: fifo, priority: 70, cpus: [3]}
)"),
    "<loaded>");

  // A shared cpu: upstream above downstream is the starvation the rule exists for.
  EXPECT_NE(
    load_error(R"(
chains:
  c:
    target: soft
    stages:
      - {node: /a, policy: fifo, priority: 80, cpus: [2, 3]}
      - {node: /b, policy: fifo, priority: 70, cpus: [3]}
)")
      .find("downstream of it on a cpu they share"),
    std::string::npos);

  // Equal on a shared cpu: FIFO does not preempt at equal priority.
  EXPECT_EQ(
    load_error(R"(
chains:
  c:
    target: soft
    stages:
      - {node: /a, policy: fifo, priority: 70, cpus: [2]}
      - {node: /b, policy: fifo, priority: 70, cpus: [2]}
)"),
    "<loaded>");
}

TEST(Spec, ReleaseStagesRunAboveTheWorkersTheyFeed)
{
  // Above the worker on the shared cpu, even though it is upstream: a release thread is not on
  // the workers' ladder.
  EXPECT_EQ(
    load_error(R"(
chains:
  c:
    target: soft
    stages:
      - {name: cam_irq, kind: release, policy: fifo, priority: 60, cpus: [2]}
      - {node: /cam, group: grab, policy: fifo, priority: 55, cpus: [2]}
)"),
    "<loaded>");

  EXPECT_NE(
    load_error(R"(
chains:
  c:
    target: soft
    stages:
      - {name: cam_irq, kind: release, policy: fifo, priority: 50, cpus: [2]}
      - {node: /cam, group: grab, policy: fifo, priority: 55, cpus: [2]}
)")
      .find("does not run above worker"),
    std::string::npos);
}

TEST(Spec, TheMoreCriticalChainComesFirstOnASharedCpu)
{
  const char * body = R"(
chains:
  control:
    target: soft
    criticality: 2
    stages:
      - {node: /ctl, policy: fifo, priority: %d, cpus: [4]}
  logging:
    target: soft
    stages:
      - {node: /log, policy: fifo, priority: 30, cpus: [4]}
)";
  char above[512];
  std::snprintf(above, sizeof above, body, 40);
  EXPECT_EQ(load_error(above), "<loaded>");

  char below[512];
  std::snprintf(below, sizeof below, body, 20);
  EXPECT_NE(load_error(below).find("more critical chain comes first"), std::string::npos);
}

TEST(Spec, AStageTakesTheHighestCriticalityOfItsChains)
{
  // /cam sits on both chains, so it is ranked with control. /log shares its cpu and must stay
  // below it even though both are listed in the logging chain.
  EXPECT_NE(
    load_error(R"(
chains:
  control:
    target: soft
    criticality: 1
    stages:
      - {node: /cam, policy: fifo, priority: 50, cpus: [2]}
  logging:
    target: soft
    stages:
      - {node: /cam, policy: fifo, priority: 50, cpus: [2]}
      - {node: /log, policy: fifo, priority: 60, cpus: [2]}
)")
      .find("more critical chain comes first"),
    std::string::npos);
}

TEST(Spec, AHardCoreCountsWorkersNotReleaseThreads)
{
  EXPECT_EQ(
    load_error(R"(
chains:
  c:
    target: hard
    stages:
      - {name: cam_irq, kind: release, policy: fifo, priority: 60, cpus: [2]}
      - {node: /cam, policy: fifo, priority: 55, cpus: [2]}
)"),
    "<loaded>");
}

// A node's default group and a group of its parent namespace are spelled the same. Every place a
// stage is named by its spelling would take the two for one, so the file is refused.
TEST(Spec, TwoStagesSpelledAlikeAreRefused)
{
  const std::string err = load_error(R"(
chains:
  c:
    target: soft
    stages:
      - {node: /foo/bar, cpus: [0]}
      - {node: /foo, group: bar, cpus: [1]}
)");
  EXPECT_NE(err.find("both named /foo/bar"), std::string::npos) << err;
}

TEST(Spec, NameStagesAreNotNodes)
{
  SpecFile f(R"(
chains:
  c:
    target: soft
    stages:
      - {name: driver_poll, policy: other}
)");
  const auto spec = capa::Spec::load(f.path());
  ASSERT_NE(spec.find("driver_poll"), nullptr);
  EXPECT_EQ(spec.find("driver_poll", "x"), nullptr) << "a name stage has no group";
  EXPECT_EQ(spec.find("/driver_poll"), nullptr) << "a leading '/' looks up a node";

  EXPECT_NE(
    load_error("chains: {c: {target: soft, stages: [{name: /x}]}}").find("must not start"),
    std::string::npos);
  EXPECT_NE(
    load_error("chains: {c: {target: soft, stages: [{name: x, group: g}]}}").find("is a name"),
    std::string::npos);
  EXPECT_NE(
    load_error("chains: {c: {target: soft, stages: [{name: x, node: /x}]}}").find("both"),
    std::string::npos);
  EXPECT_NE(
    load_error("chains: {c: {target: soft, stages: [{policy: other}]}}").find("neither"),
    std::string::npos);
}

TEST(Spec, ChildrenAreDeclaredPerStage)
{
  SpecFile f(R"(
chains:
  c:
    target: soft
    stages:
      - {node: /omp, policy: fifo, priority: 20, children: inherit}
)");
  EXPECT_EQ(capa::Spec::load(f.path()).stage("/omp").opts.children, capa::Children::Inherit);

  EXPECT_NE(
    load_error("chains: {c: {target: soft, stages: [{node: /a, children: inherit}]}}")
      .find("would be ignored"),
    std::string::npos)
    << "inherit with no RT policy passes nothing on";
  EXPECT_NE(
    load_error("chains: {c: {target: soft, stages: [{node: /a, external: true, children: reset}]}}")
      .find("must not carry"),
    std::string::npos);
}

TEST(Spec, TwoChainsMustAgreeOnKindAndChildren)
{
  EXPECT_NE(
    load_error(R"(
chains:
  a:
    target: soft
    stages:
      - {node: /s, policy: fifo, priority: 20}
  b:
    target: soft
    stages:
      - {node: /s, policy: fifo, priority: 20, children: inherit}
)")
      .find("declared differently"),
    std::string::npos);
}

// ---- bind and spawn: the stage by name, from CAPA_SPEC ----

namespace
{

struct SpecEnv
{
  explicit SpecEnv(const std::string & path) { ::setenv("CAPA_SPEC", path.c_str(), 1); }
  SpecEnv(const SpecEnv &) = delete;
  SpecEnv & operator=(const SpecEnv &) = delete;
  ~SpecEnv() { ::unsetenv("CAPA_SPEC"); }
};

std::string pin_spec(std::uint32_t cpu)
{
  return "chains:\n  c:\n    target: soft\n    stages:\n      - node: /bind_node\n"
         "        group: work\n        cpus: [" +
         std::to_string(cpu) + "]\n";
}

}  // namespace

TEST(Bind, WithoutASpecItDoesNothing)
{
  ::unsetenv("CAPA_SPEC");
  const auto before = capa::current();
  std::thread t([] { EXPECT_TRUE(capa::bind("/bind_node", "work").findings.empty()); });
  t.join();
  EXPECT_EQ(capa::current().cpus, before.cpus);
}

TEST(Bind, PutsTheCallingThreadOnItsStage)
{
  const auto allowed = capa::current().cpus;
  ASSERT_FALSE(allowed.empty());
  SpecFile f(pin_spec(allowed.back()));
  SpecEnv env(f.path());

  std::vector<std::uint32_t> got;
  std::thread t([&] {
    capa::bind("/bind_node", "work");
    got = capa::current().cpus;
  });
  t.join();
  EXPECT_EQ(got, std::vector<std::uint32_t>{allowed.back()});
}

TEST(Bind, ATypoIsRefusedNotIgnored)
{
  SpecFile f(pin_spec(0));
  SpecEnv env(f.path());
  std::thread t([] { EXPECT_THROW(capa::bind("/bind_node", "wrok"), std::runtime_error); });
  t.join();
}

TEST(Spawn, RunsTheFunctionOnlyAfterTheThreadIsSet)
{
  const auto allowed = capa::current().cpus;
  ASSERT_FALSE(allowed.empty());
  SpecFile f(pin_spec(allowed.back()));
  SpecEnv env(f.path());

  std::vector<std::uint32_t> first_look;
  std::thread t = capa::spawn("/bind_node", "work", [&] { first_look = capa::current().cpus; });
  t.join();
  EXPECT_EQ(first_look, std::vector<std::uint32_t>{allowed.back()})
    << "the function ran before the stage was applied";
}

TEST(Spawn, ARefusalReachesTheCallerAndTheFunctionNeverRuns)
{
  SpecFile f(pin_spec(0));
  SpecEnv env(f.path());
  bool ran = false;
  EXPECT_THROW(capa::spawn("/bind_node", "wrok", [&] { ran = true; }), std::runtime_error);
  EXPECT_FALSE(ran);
}

// The file checked the ordering when it was loaded; the per-thread report says so instead of
// asking for a control priority nobody passed.
TEST(Bind, TheReportCarriesTheFileOrderingAsChecked)
{
  SpecFile f(R"(
chains:
  c:
    target: soft
    stages:
      - {node: /ord, group: a, policy: fifo, priority: 10}
      - {node: /ord, group: b, policy: fifo, priority: 20}
)");
  const auto spec = capa::Spec::load(f.path());
  const capa::Report host = capa::preflight(spec.stage("/ord", "a").opts);
  const capa::Finding * perm = host.find("policy-permission");
  ASSERT_NE(perm, nullptr);
  if (perm->verdict != capa::Verdict::Ok) GTEST_SKIP() << "capa-cap:rtprio no SCHED_FIFO here";

  capa::Report rep;
  std::thread t([&] { rep = capa::apply_checked(spec.stage("/ord", "a")); });
  t.join();
  ASSERT_NE(rep.find("priority-order"), nullptr);
  EXPECT_EQ(rep.find("priority-order")->verdict, capa::Verdict::Ok) << rep.to_string();
}

// The user document shows chain files. A fence that no longer loads teaches a file the parser
// refuses, so every yaml fence in them is loaded here.
TEST(Docs, EveryChainFileInTheUserDocumentsLoads)
{
  int fences = 0;
  for (const char * doc : {"/api.md"}) {
    std::ifstream in(std::string(CAPA_DOCS_DIR) + doc);
    ASSERT_TRUE(in) << doc;
    std::string line;
    std::string fence;
    bool inside = false;
    while (std::getline(in, line)) {
      if (!inside && line == "```yaml") {
        inside = true;
        fence.clear();
      } else if (inside && line == "```") {
        inside = false;
        ++fences;
        SpecFile f(fence);
        EXPECT_NO_THROW(capa::Spec::load(f.path())) << doc << ":\n" << fence;
      } else if (inside) {
        fence += line + "\n";
      }
    }
  }
  EXPECT_GE(fences, 4) << "the document lost its examples";
}

TEST(Spec, EqualCriticalityOrDisjointCpusBindNoOrderBetweenChains)
{
  // Same criticality: neither chain comes first, so the lower-priority stage may be either.
  EXPECT_EQ(
    load_error(R"(
chains:
  a:
    target: soft
    stages:
      - {node: /a, policy: fifo, priority: 20, cpus: [4]}
  b:
    target: soft
    stages:
      - {node: /b, policy: fifo, priority: 30, cpus: [4]}
)"),
    "<loaded>");

  // More critical but on another cpu: the two never compete.
  EXPECT_EQ(
    load_error(R"(
chains:
  a:
    target: soft
    criticality: 5
    stages:
      - {node: /a, policy: fifo, priority: 20, cpus: [4]}
  b:
    target: soft
    stages:
      - {node: /b, policy: fifo, priority: 30, cpus: [5]}
)"),
    "<loaded>");
}

// An external stage's cpus are not in the file, so it may land next to any worker of its chain,
// and a release thread has to clear all of them.
TEST(Spec, AnExternalReleaseStageMustClearEveryWorkerOfItsChain)
{
  EXPECT_NE(
    load_error(R"(
chains:
  c:
    target: soft
    stages:
      - {name: nic_irq, kind: release, external: true, expect_priority: 60}
      - {node: /rx, policy: fifo, priority: 55, cpus: [2]}
      - {node: /ctl, policy: fifo, priority: 70, cpus: [3]}
)")
      .find("does not run above worker /ctl"),
    std::string::npos);
}

TEST(Spec, AMatchNamesAnInterruptOrAProcessAndThread)
{
  EXPECT_EQ(
    load_error("chains: {c: {target: soft, stages: [{name: usb, kind: release, match: "
               "{irq: 3550000.usb}, policy: fifo, priority: 60, cpus: [2]}]}}"),
    "<loaded>");
  EXPECT_EQ(
    load_error("chains: {c: {target: soft, stages: [{name: rx, match: {process: cam, "
               "thread: dds.recv}, cpus: [2]}]}}"),
    "<loaded>");

  EXPECT_EQ(
    load_error("chains: {c: {target: soft, stages: [{name: rx, match: {arg: "
               "\"__node:=planning\", thread: recvUC}, cpus: [2]}]}}"),
    "<loaded>");
  EXPECT_EQ(
    load_error("chains: {c: {target: soft, stages: [{name: rx, match: {process: agnocast_compon, "
               "arg: \"__node:=planning\", thread: recvUC}, cpus: [2]}]}}"),
    "<loaded>");

  for (const char * bad : {
         "{name: a, match: {irq: x, thread: y}, cpus: [2]}",          // both forms
         "{name: a, match: {process: cam}, cpus: [2]}",               // half of one
         "{name: a, match: {arg: x}, cpus: [2]}",                     // no thread
         "{name: a, match: {irq: x, arg: y}, cpus: [2]}",             // an irq has no argv
         "{name: a, match: {arg: \"a\\tb\", thread: y}, cpus: [2]}",  // not a state file field
         "{name: a, match: {irq: x, prio: 3}, cpus: [2]}",            // unknown key
         "{name: a, match: {irq: x}}",                                // nothing to set
         "{name: a, external: true, match: {irq: x}}",                // nobody sets an external one
       }) {
    EXPECT_NE(
      load_error(std::string("chains: {c: {target: soft, stages: [") + bad + "]}}"), "<loaded>")
      << bad;
  }
}

TEST(Spec, ChildrenCpusMovesResetChildrenOffTheStagesOwnCpus)
{
  EXPECT_EQ(
    load_error("chains: {c: {target: soft, stages: [{node: /a, cpus: [2], "
               "children_cpus: [0, 1]}]}}"),
    "<loaded>");
  for (const char * bad : {
         "{node: /a, children_cpus: [0]}",             // no cpus to move off
         "{node: /a, cpus: [2], children_cpus: [2]}",  // onto the same cpu
         "{node: /a, policy: fifo, priority: 20, cpus: [2], children: inherit, children_cpus: [0]}",
       }) {
    EXPECT_NE(
      load_error(std::string("chains: {c: {target: soft, stages: [") + bad + "]}}"), "<loaded>")
      << bad;
  }
}

// A matched stage is the daemon's to set; a thread binding it as well would give it two owners.
TEST(Bind, AMatchedStageIsRefused)
{
  SpecFile f(
    "chains: {c: {target: soft, stages: [{name: rx, match: {process: cam, thread: "
    "dds.recv}, cpus: [0]}]}}\n");
  const auto spec = capa::Spec::load(f.path());
  std::thread t(
    [&] { EXPECT_THROW(capa::apply_checked(spec.stage("rx")), std::invalid_argument); });
  t.join();
}

TEST(Bind, AFailedRebindKeepsTheStageItHeld)
{
  const auto allowed = capa::current().cpus;
  ASSERT_FALSE(allowed.empty());
  SpecFile f(
    "chains: {c: {target: soft, stages: [{node: /rebind, group: a, cpus: [" +
    std::to_string(allowed.back()) + "]}, {node: /rebind, group: b, cpus: [1000]}]}}\n");
  const auto spec = capa::Spec::load(f.path());
  std::thread t([&] {
    capa::apply_checked(spec.stage("/rebind", "a"));
    EXPECT_ANY_THROW(capa::apply_checked(spec.stage("/rebind", "b")));
    std::thread other([&] {
      EXPECT_THROW(capa::apply_checked(spec.stage("/rebind", "a")), std::invalid_argument);
    });
    other.join();
  });
  t.join();
}

TEST(Verify, AWindowReadsTheThreadsAndItsCpusCounters)
{
  const SpecFile file(R"(
chains:
  a:
    target: soft
    stages:
      - {name: toucher, policy: other}
)");
  const capa::Stage stage = capa::Spec::load(file.path()).stage("toucher");
  std::promise<int> tid;
  std::atomic<bool> stop{false};
  std::thread t([&] {
    tid.set_value(capa::this_tid());
    while (!stop) {
      std::vector<char> fresh(1 << 20);
      for (std::size_t i = 0; i < fresh.size(); i += 4096) fresh[i] = 1;
      std::this_thread::yield();
    }
  });
  const int worker = tid.get_future().get();

  const auto now = capa::verify(stage, worker);
  EXPECT_EQ(now.find("thread-faults"), nullptr) << "counters need a window";
  ASSERT_NE(now.find("pi-boosted"), nullptr);
  EXPECT_EQ(now.find("pi-boosted")->verdict, capa::Verdict::Ok);
  ASSERT_NE(now.find("timerslack"), nullptr) << "another thread's needs CAP_SYS_NICE, so Unknown";

  const auto window = capa::verify(stage, worker, std::chrono::milliseconds(200));
  stop = true;
  t.join();
  for (const char * id :
       {"thread-faults", "thread-migrations", "thread-preemptions", "cpu-interrupts",
        "cpu-softirqs", "cgroup-throttle"}) {
    EXPECT_NE(window.find(id), nullptr) << id << "\n" << window.to_string();
  }
  const auto * faults = window.find("thread-faults");
  ASSERT_NE(faults, nullptr);
  EXPECT_EQ(faults->detail.rfind("0 minor", 0), std::string::npos)
    << "the worker faulted in fresh pages throughout the window: " << faults->detail;
}

TEST(VerifyProcess, NamesIoUringHelperThreads)
{
  const SpecFile file(R"(
chains:
  a:
    target: soft
    stages:
      - {name: plain, policy: other}
)");
  const capa::Spec spec = capa::Spec::load(file.path());
  EXPECT_EQ(capa::verify_process(spec).find("io-uring")->verdict, capa::Verdict::Ok);

  std::promise<void> named;
  std::promise<void> done;
  std::thread t([&] {
    ::pthread_setname_np(::pthread_self(), "iou-wrk-1");
    named.set_value();
    done.get_future().wait();
  });
  named.get_future().wait();
  const auto rep = capa::verify_process(spec);
  done.set_value();
  t.join();
  ASSERT_NE(rep.find("io-uring"), nullptr);
  EXPECT_EQ(rep.find("io-uring")->verdict, capa::Verdict::Warn);
}

TEST(Spec, HoldsNamesAGroupByWhatItHolds)
{
  EXPECT_EQ(
    load_error("chains: {c: {target: soft, stages: [{node: /mvp, group: traj, holds: "
               "{subscription: /traj}, cpus: [2]}, {node: /mvp, group: srv, holds: "
               "{service: /mvp/set}, cpus: [2]}]}}"),
    "<loaded>");
  for (const char * bad : {
         "{node: /a, holds: {subscription: /t}}",                         // no label
         "{name: a, holds: {subscription: /t}}",                          // not a node
         "{node: /a, group: g, holds: {subscription: t}}",                // not resolved
         "{node: /a, group: g, holds: {subscription: /t, service: /s}}",  // one thing
         "{node: /a, group: g, holds: {}}",
         "{node: /a, group: g, holds: {timer: 100}}",
         "{node: /a, group: g, external: true, holds: {subscription: /t}}",
         // NOLINTNEXTLINE(bugprone-suspicious-missing-comma): one entry, split for length
         "{node: /a, group: g, holds: {subscription: /t}, match: {process: p, thread: t}, "
         "cpus: [2]}",
       }) {
    EXPECT_NE(
      load_error(std::string("chains: {c: {target: soft, stages: [") + bad + "]}}"), "<loaded>")
      << bad;
  }
  EXPECT_NE(
    load_error("chains: {c: {target: soft, stages: [{node: /a, group: x, holds: {subscription: "
               "/t}}, {node: /a, group: y, holds: {subscription: /t}}]}}")
      .find("hold the same thing"),
    std::string::npos);
}

namespace
{
capa::GroupContent group(std::vector<std::string> subs, bool is_default = false)
{
  capa::GroupContent g;
  g.is_default = is_default;
  g.subscriptions = std::move(subs);
  return g;
}

constexpr const char * kPlanner = R"(
chains:
  c:
    target: soft
    stages:
      - {node: /mvp, cpus: [0]}
      - {node: /mvp, group: traj, holds: {subscription: /path_optimizer/trajectory}, cpus: [0]}
      - {node: /mvp, group: by_label, cpus: [0]}
      - {node: /mvp, group: ext, external: true}
      - {node: /other, holds: {subscription: /x}, group: x, cpus: [0]}
)";
}  // namespace

// Autoware's motion_velocity_planner did its work in its eighth group; numbering groups by
// creation order named it "cg7", and one more group in the node code would move that silently.
TEST(Spec, DeclaredFindsAGroupByItsContentNotItsPosition)
{
  SpecFile f(kPlanner);
  const auto spec = capa::Spec::load(f.path());
  std::vector<capa::GroupContent> groups = {
    group({"/odom"}, true), group({"/map"}), group({"/path_optimizer/trajectory"})};
  auto got = spec.declared("/mvp", groups);
  ASSERT_EQ(got.size(), 3u);
  ASSERT_NE(got[0], nullptr);
  EXPECT_EQ(got[0]->group, "");
  EXPECT_EQ(got[1], nullptr) << "a group no stage declares";
  ASSERT_NE(got[2], nullptr);
  EXPECT_EQ(got[2]->group, "traj");

  groups.insert(groups.begin() + 1, group({"/new"}));
  got = spec.declared("/mvp", groups);
  ASSERT_NE(got[3], nullptr);
  EXPECT_EQ(got[3]->group, "traj") << "one more group does not move the stage";

  EXPECT_EQ(spec.declared("/nobody", groups), std::vector<const capa::Stage *>(4, nullptr));
}

TEST(Spec, DeclaredRefusesToGuess)
{
  SpecFile f(kPlanner);
  const auto spec = capa::Spec::load(f.path());
  EXPECT_THROW(
    spec.declared(
      "/mvp", {group({}, true), group({"/path_optimizer/trajectory"}),
               group({"/path_optimizer/trajectory"})}),
    std::runtime_error)
    << "two groups hold the subscription";
  EXPECT_THROW(
    spec.declared("/mvp", {group({"/path_optimizer/trajectory"}, true)}), std::runtime_error)
    << "the default group is claimed by the default stage and by the holds stage";
}

TEST(Bind, BindDeclaredBindsOnlyWhatTheFileDeclares)
{
  const auto allowed = capa::current().cpus;
  ASSERT_FALSE(allowed.empty());
  ::unsetenv("CAPA_SPEC");
  std::thread([] { EXPECT_FALSE(capa::bind_declared("/bind_node", "work")); }).join();

  SpecFile f(
    pin_spec(allowed.back()) +
    "      - {node: /bind_node, group: ext, external: true}\n"
    "      - {name: rx, match: {process: p, thread: t}, cpus: [0]}\n");
  SpecEnv env(f.path());
  std::vector<std::uint32_t> before;
  std::vector<std::uint32_t> after;
  std::thread([&] {
    before = capa::current().cpus;
    EXPECT_FALSE(capa::bind_declared("/bind_node", "wrok"));
    EXPECT_FALSE(capa::bind_declared("/bind_node", "ext"));
    EXPECT_FALSE(capa::bind_declared("rx"));
    after = capa::current().cpus;
  }).join();
  EXPECT_EQ(after, before) << "nothing undeclared was applied";

  std::thread([&] {
    const auto rep = capa::bind_declared("/bind_node", "work");
    EXPECT_TRUE(rep.has_value());
    after = capa::current().cpus;
  }).join();
  EXPECT_EQ(after, std::vector<std::uint32_t>{allowed.back()});
}
