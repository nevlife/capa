#include "capa/rt.hpp"
#include "result_test_util.hpp"

#include <gtest/gtest.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <system_error>
#include <thread>

// Covers the RT opt-in surface: validate rejects nonsense, apply is
// all-or-nothing on the calling thread, and preflight's permission verdict agrees with
// what apply then does -- so the tests hold both with and without RT privileges.

namespace
{

// Tests mutate the calling thread's scheduling; put it back so later tests inherit a
// clean thread whichever assertions fired.
struct RestoreThread
{
  capa::ThreadState saved = capa::current();

  RestoreThread() = default;
  RestoreThread(const RestoreThread &) = delete;
  auto operator=(const RestoreThread &) -> RestoreThread & = delete;
  ~RestoreThread()
  {
    // SCHED_RESET_ON_FORK, once set, clears only with CAP_SYS_NICE. Keep it rather than fail the
    // whole restore: on a SCHED_OTHER thread it changes nothing the next test looks at.
    sched_param param{};
    param.sched_priority = saved.priority;
    const int keep = capa::current().reset_on_fork ? SCHED_RESET_ON_FORK : 0;
    ::sched_setscheduler(0, saved.policy | keep, &param);
    cpu_set_t set;
    CPU_ZERO(&set);
    for (auto c : saved.cpus) {
      CPU_SET(static_cast<int>(c), &set);
    }
    ::sched_setaffinity(0, sizeof set, &set);
  }
};

}  // namespace

TEST(Rt, ValidateRejectsReinterpretableCombos)
{
  EXPECT_TRUE((capa::Options{}.validate()).ok());

  capa::Options fifo_no_prio;
  fifo_no_prio.policy = capa::Policy::Fifo;
  EXPECT_EQ(kind_of(fifo_no_prio.validate()), capa::ErrorKind::InvalidArgument);

  capa::Options prio_no_policy;
  prio_no_policy.priority = 10;
  EXPECT_EQ(kind_of(prio_no_policy.validate()), capa::ErrorKind::InvalidArgument);

  capa::Options over;
  over.policy = capa::Policy::RoundRobin;
  over.priority = 100;
  EXPECT_EQ(kind_of(over.validate()), capa::ErrorKind::InvalidArgument);

  capa::Options bad_cpu;
  bad_cpu.cpus = {100000};
  EXPECT_EQ(kind_of(bad_cpu.validate()), capa::ErrorKind::InvalidArgument);
}

TEST(Rt, ApplyOfDefaultOptionsChangesNothing)
{
  const auto before = capa::current();
  must(capa::detail::apply(capa::Options{}));
  const auto after = capa::current();
  EXPECT_EQ(before.policy, after.policy);
  EXPECT_EQ(before.priority, after.priority);
  EXPECT_EQ(before.cpus, after.cpus);
}

TEST(Rt, ApplyPinsTheCallingThread)
{
  RestoreThread restore;
  const auto before = capa::current();
  ASSERT_FALSE(before.cpus.empty());
  capa::Options opts;
  opts.cpus = {before.cpus.front()};
  must(capa::detail::apply(opts));
  EXPECT_EQ(capa::current().cpus, opts.cpus);
}

TEST(Rt, ApplyMatchesPreflightPermissionVerdict)
{
  RestoreThread restore;
  capa::Options opts;
  opts.policy = capa::Policy::Fifo;
  opts.priority = 10;
  const auto rep = must(capa::preflight(opts));
  const auto * perm = rep.find("policy-permission");
  ASSERT_NE(perm, nullptr);
  if (perm->verdict == capa::Verdict::Ok) {
    must(capa::detail::apply(opts));
    const auto state = capa::current();
    EXPECT_EQ(state.policy, SCHED_FIFO);
    EXPECT_EQ(state.priority, 10);
  } else {
    EXPECT_EQ(kind_of(capa::detail::apply(opts)), capa::ErrorKind::System);
    EXPECT_NE(capa::current().policy, SCHED_FIFO);
  }
}

TEST(Rt, RefusedApplyRollsBackAffinity)
{
  RestoreThread restore;
  const auto before = capa::current();
  ASSERT_FALSE(before.cpus.empty());
  capa::Options opts;
  opts.cpus = {before.cpus.front()};
  opts.policy = capa::Policy::Fifo;
  opts.priority = 10;
  const auto rep = must(capa::preflight(opts));
  const auto * perm = rep.find("policy-permission");
  ASSERT_NE(perm, nullptr);
  if (perm->verdict == capa::Verdict::Ok) {
    must(capa::detail::apply(opts));
    EXPECT_EQ(capa::current().policy, SCHED_FIFO);
    EXPECT_EQ(capa::current().cpus, opts.cpus);
  } else {
    EXPECT_EQ(kind_of(capa::detail::apply(opts)), capa::ErrorKind::System);
    EXPECT_EQ(capa::current().cpus, before.cpus);
  }
}

TEST(Rt, PreflightAlwaysReportsHostFindings)
{
  const auto rep = must(capa::preflight(capa::Options{}));
  EXPECT_NE(rep.find("kernel-preemption"), nullptr);
  EXPECT_NE(rep.find("memory-lock"), nullptr);
  EXPECT_EQ(rep.find("policy-permission"), nullptr);
  EXPECT_EQ(rep.find("cpu-online"), nullptr);
  EXPECT_FALSE(rep.to_string().empty());
}

TEST(Rt, PreflightChecksRequestSpecificItems)
{
  capa::Options opts;
  opts.policy = capa::Policy::Fifo;
  opts.priority = 20;
  opts.cpus = {0};
  const auto rep = must(capa::preflight(opts));
  for (const char * id :
       {"policy-permission", "rt-throttle", "cpu-online", "cpu-isolation", "cpu-governor"}) {
    EXPECT_NE(rep.find(id), nullptr) << id;
  }
  ASSERT_NE(rep.find("cpu-online"), nullptr);
  EXPECT_EQ(rep.find("cpu-online")->verdict, capa::Verdict::Ok);
}

TEST(Rt, PreflightRejectsTransportAtOrAboveControl)
{
  capa::Options opts;
  opts.policy = capa::Policy::Fifo;
  opts.priority = 80;
  const auto above = must(capa::preflight(opts, /*control_priority=*/50));
  const auto * order = above.find("priority-order");
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->verdict, capa::Verdict::Fail);
  EXPECT_FALSE(above.ok());

  opts.priority = 30;
  const auto below = must(capa::preflight(opts, /*control_priority=*/50));
  ASSERT_NE(below.find("priority-order"), nullptr);
  EXPECT_EQ(below.find("priority-order")->verdict, capa::Verdict::Ok);
}

TEST(Rt, PreflightFailsOfflineCpu)
{
  if (std::thread::hardware_concurrency() >= 1000) {
    GTEST_SKIP() << "capa-cap:offline-cpu no cpu id that is safely below the set size yet offline";
  }
  capa::Options opts;
  opts.cpus = {1023};
  const auto rep = must(capa::preflight(opts));
  const auto * online = rep.find("cpu-online");
  ASSERT_NE(online, nullptr);
  EXPECT_EQ(online->verdict, capa::Verdict::Fail);
  EXPECT_FALSE(rep.ok());
}

TEST(Rt, UndeclaredControlPriorityIsReportedNotSkipped)
{
  // A check that did not run and a check that passed must not look alike. Omitting the finding
  // made an undeclared control loop indistinguishable from a verified ordering.
  capa::Options opts;
  opts.policy = capa::Policy::Fifo;
  opts.priority = 80;

  const auto rep = must(capa::preflight(opts));  // control_priority left undeclared
  const auto * order = rep.find("priority-order");
  ASSERT_NE(order, nullptr) << "the ordering check vanished instead of reporting itself";
  EXPECT_EQ(order->verdict, capa::Verdict::Unknown)
    << "Unknown is not a failure; it is an unanswered question. rep.ok() is not asserted here "
       "because policy-permission decides it on hosts without RT privileges";
}

TEST(Rt, ApplyCheckedIsANoOpForOptionsThatAskNothing)
{
  RestoreThread restore;
  const auto before = capa::current();
  const auto rep = must(capa::apply_checked(capa::Options{}, capa::Strictness::Hard));
  EXPECT_TRUE(rep.findings.empty()) << "an empty request has no host to judge";
  const auto after = capa::current();
  EXPECT_EQ(after.policy, before.policy);
  EXPECT_EQ(after.priority, before.priority);
}

TEST(Rt, ApplyCheckedRefusesWhatPreflightFails)
{
  RestoreThread restore;
  if (std::thread::hardware_concurrency() >= 1000) {
    GTEST_SKIP() << "capa-cap:offline-cpu no cpu id that is safely below the set size yet offline";
  }
  capa::Options opts;
  opts.cpus = {1023};  // offline: preflight Fails on cpu-online
  const auto before = capa::current();

  EXPECT_EQ(kind_of(capa::apply_checked(opts, capa::Strictness::Soft)), capa::ErrorKind::Failed);

  const auto after = capa::current();
  EXPECT_EQ(after.cpus, before.cpus) << "a refused apply_checked must not have touched the thread";
}

TEST(Rt, HardRefusesTheWarnsSoftTolerates)
{
  RestoreThread restore;
  // The host findings (kernel preemption, memory lock) are always evaluated, so a stock kernel
  // yields Warns. Strictness is the only thing that separates the two readings of them.
  capa::Options opts;
  opts.cpus = {0};
  const auto rep = must(capa::preflight(opts));
  if (!rep.ok()) {
    GTEST_SKIP()
      << "capa-cap:rt-warn-verdict this host Fails before Warn strictness can be observed";
  }

  bool warned = false;
  for (const auto & f : rep.findings) {
    if (f.verdict == capa::Verdict::Warn) {
      warned = true;
    }
  }
  if (!warned) {
    GTEST_SKIP()
      << "capa-cap:rt-warn-verdict this host is fully configured for RT; no Warn to strengthen";
  }

  EXPECT_TRUE((capa::apply_checked(opts, capa::Strictness::Soft)).ok());
  EXPECT_EQ(kind_of(capa::apply_checked(opts, capa::Strictness::Hard)), capa::ErrorKind::Failed);
}

TEST(Rt, PreflightJudgesAllFourIsolationAxes)
{
  // Isolation is four boot-time settings, and preflight used to look at two. A core that is in
  // isolcpus but still receives interrupts is not isolated for latency purposes, and reporting Ok
  // on the two it read let a half-configured host pass -- which is what `target: hard` is supposed
  // to refuse.
  capa::Options opts;
  opts.cpus = {0};
  const auto rep = must(capa::preflight(opts));

  for (const char * id : {"cpu-isolation", "rcu-offload", "irq-affinity"}) {
    const auto * f = rep.find(id);
    ASSERT_NE(f, nullptr) << id << " is not judged";
    EXPECT_NE(f->verdict, capa::Verdict::Fail) << id << " must not be fatal on a stock host";
    EXPECT_FALSE(f->detail.empty()) << id << " says nothing about why";
  }
}

TEST(Rt, IsolationAxesAreNotJudgedWithoutARequestedCpu)
{
  // They answer "is the cpu you asked for prepared", so with no cpu requested there is nothing to
  // answer. Reporting Ok would claim a check that never had a subject.
  capa::Options opts;
  opts.policy = capa::Policy::Fifo;
  opts.priority = 10;
  const auto rep = must(capa::preflight(opts));

  EXPECT_EQ(rep.find("rcu-offload"), nullptr);
  EXPECT_EQ(rep.find("irq-affinity"), nullptr);
  EXPECT_EQ(rep.find("cpu-isolation"), nullptr);
}

// Reading another thread is the half of the setting/reading asymmetry that makes a chain
// checkable end to end. The claim is that observe() answers about the thread it
// was handed and not about the caller, so the child reports itself through current() and the
// parent -- running at a different policy on purpose -- must arrive at the same answer.
TEST(Rt, ObserveAnswersAboutTheThreadItWasHanded)
{
  capa::Options opts;
  opts.policy = capa::Policy::Fifo;
  opts.priority = 12;
  const auto rep = must(capa::preflight(opts));
  const auto * perm = rep.find("policy-permission");
  ASSERT_NE(perm, nullptr);

  std::atomic<int> child_tid{0};
  std::atomic<bool> settled{false};
  std::atomic<bool> stop{false};
  capa::ThreadState self_view;
  std::thread child([&] {
    if (perm->verdict == capa::Verdict::Ok) {
      must(capa::detail::apply(opts));
    }
    self_view = capa::current();
    child_tid.store(capa::this_tid());
    settled.store(true);
    while (!stop.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });
  while (!settled.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  const int tid = child_tid.load();
  EXPECT_NE(tid, capa::this_tid()) << "this_tid returned the same id on two threads";
  const capa::ThreadState outside_view = must(capa::observe(tid));
  stop.store(true);
  child.join();

  EXPECT_EQ(outside_view.policy, self_view.policy);
  EXPECT_EQ(outside_view.priority, self_view.priority);
  EXPECT_EQ(outside_view.cpus, self_view.cpus);
  // The caller is deliberately not the observed thread; a read that ignored its argument would
  // have matched the parent instead, and on the privileged branch those differ.
  if (perm->verdict == capa::Verdict::Ok) {
    EXPECT_EQ(outside_view.policy, SCHED_FIFO);
    EXPECT_EQ(outside_view.priority, 12);
    EXPECT_NE(outside_view.policy, capa::current().policy);
  }
}

// A thread that is not there must not read as a thread at rest. SCHED_OTHER with priority 0 is
// what an unset thread looks like, so answering it for a tid the kernel does not know would turn
// "this stage never started" into "this stage is fine".
TEST(Rt, ObserveRefusesWhatItCannotRead)
{
  EXPECT_EQ(kind_of(capa::observe(0)), capa::ErrorKind::InvalidArgument);
  EXPECT_EQ(kind_of(capa::observe(-1)), capa::ErrorKind::InvalidArgument);

  std::ifstream f("/proc/sys/kernel/pid_max");
  int pid_max = 0;
  ASSERT_TRUE(f >> pid_max) << "cannot read pid_max";
  // Above the wrap point, so it names no thread now and cannot be allocated later either --
  // unlike the tid of a thread this test just joined, which the kernel is free to hand out again.
  EXPECT_EQ(kind_of(capa::observe(pid_max + 1)), capa::ErrorKind::System);
}

// Refuses a vacuous pass. Every test above holds on both sides of the permission branch, so this
// suite is green on a host that never once applied SCHED_FIFO -- the default state of any machine
// whose rtprio ulimit nobody raised. A run that never reached the state under test proves
// nothing about it. Nothing readable from the host says whether it
// was MEANT to be RT-capable, so the environment declares it -- the same shape as the chain file's
// `target`, where declaring is what turns a report nobody reads into a refusal.
TEST(Rt, PrivilegedPathIsExercisedWhereDeclared)
{
  capa::Options opts;
  opts.policy = capa::Policy::Fifo;
  opts.priority = 10;
  const auto rep = must(capa::preflight(opts));
  const auto * perm = rep.find("policy-permission");
  ASSERT_NE(perm, nullptr);
  const bool privileged = perm->verdict == capa::Verdict::Ok;

  if (std::getenv("CAPA_REQUIRE_PRIVILEGE") != nullptr) {
    EXPECT_TRUE(privileged)
      << "CAPA_REQUIRE_PRIVILEGE declares this host RT-capable, but every RT test here could "
         "only watch apply() be refused:\n  "
      << perm->detail;
    return;
  }
  if (!privileged) {
    std::cout << "[  NOTE    ] RT tests took the refusal branch only: " << perm->detail << "\n"
              << "[  NOTE    ] set CAPA_REQUIRE_PRIVILEGE=1 where SCHED_FIFO must work\n";
  }
}

TEST(Rt, ValidateRejectsInheritedChildrenWithoutAnRtPolicy)
{
  capa::Options opts;
  opts.children = capa::Children::Inherit;
  EXPECT_EQ(kind_of(opts.validate()), capa::ErrorKind::InvalidArgument) << "nothing RT to pass on";
  opts.policy = capa::Policy::Fifo;
  opts.priority = 10;
  EXPECT_TRUE((opts.validate()).ok());
}

// Linux hands a child its parent's policy by default, pthreads included. Reset must reach a
// std::thread child, not only fork(); Inherit must leave the default alone. Affinity is inherited
// either way, which the kernel offers no flag against.
TEST(Rt, ChildrenFollowTheDeclaredPolicy)
{
  capa::Options opts;
  opts.policy = capa::Policy::Fifo;
  opts.priority = 10;
  const auto rep = must(capa::preflight(opts));
  const auto * perm = rep.find("policy-permission");
  ASSERT_NE(perm, nullptr);
  if (perm->verdict != capa::Verdict::Ok) {
    GTEST_SKIP() << "capa-cap:rtprio host does not permit SCHED_FIFO";
  }

  for (auto children : {capa::Children::Reset, capa::Children::Inherit}) {
    capa::ThreadState parent;
    capa::ThreadState child;
    std::thread outer([&] {
      opts.children = children;
      must(capa::apply_checked(opts, capa::Strictness::Soft));
      parent = capa::current();
      std::thread inner([&] { child = capa::current(); });
      inner.join();
    });
    outer.join();

    EXPECT_EQ(parent.policy, SCHED_FIFO);
    EXPECT_EQ(parent.reset_on_fork, children == capa::Children::Reset);
    if (children == capa::Children::Reset) {
      EXPECT_EQ(child.policy, SCHED_OTHER) << "a reset stage leaked FIFO to its child";
      EXPECT_EQ(child.priority, 0);
    } else {
      EXPECT_EQ(child.policy, SCHED_FIFO) << "an inherit stage's child did not inherit";
      EXPECT_EQ(child.priority, 10);
    }
    EXPECT_FALSE(child.reset_on_fork) << "the flag is not itself inherited";
  }
}

// The daemon judges threads of other processes. A page fault stalls the process that takes it, so
// memory-lock is read from the target's process, and a kernel thread has no user memory at all.
TEST(Preflight, MemoryLockIsJudgedOnTheTargetsProcess)
{
  capa::Options o;
  o.cpus = {0};
  int fds[2];
  ASSERT_EQ(::pipe(fds), 0);
  const pid_t pid = ::fork();
  if (pid == 0) {
    ::close(fds[0]);
    static char page[4096];
    // A sanitizer build intercepts mlock and locks nothing, so the kernel's count decides.
    const bool locked =
      ::mlock(page, sizeof page) == 0 &&
      must(capa::detail::preflight_to(0, o, 0)).find("memory-lock")->verdict == capa::Verdict::Ok;
    const char c = locked ? 'y' : 'n';
    if (::write(fds[1], &c, 1) != 1) {
      ::_exit(1);
    }
    for (;;) {
      ::pause();
    }
  }
  ::close(fds[1]);
  char c = 0;
  ASSERT_EQ(::read(fds[0], &c, 1), 1);
  ::close(fds[0]);
  const capa::Report theirs = must(capa::detail::preflight_to(pid, o, 0));
  ::kill(pid, SIGKILL);
  ::waitpid(pid, nullptr, 0);
  if (c != 'y') {
    GTEST_SKIP() << "capa-cap:memlock no page could be locked (host limit or sanitizer)";
  }

  const capa::Report mine = must(capa::preflight(o));
  ASSERT_NE(theirs.find("memory-lock"), nullptr) << theirs.to_string();
  EXPECT_EQ(theirs.find("memory-lock")->verdict, capa::Verdict::Ok) << theirs.to_string();
  EXPECT_EQ(mine.find("memory-lock")->verdict, capa::Verdict::Warn)
    << "this test process locks no memory, so its own verdict must differ";

  const capa::Report kthread = must(capa::detail::preflight_to(2, o, 0));  // kthreadd
  ASSERT_NE(kthread.find("memory-lock"), nullptr);
  EXPECT_EQ(kthread.find("memory-lock")->verdict, capa::Verdict::Ok) << kthread.to_string();
}

TEST(Result, UnwrapRaisesTheExceptionItsCallersKnow)
{
  using capa::detail::unwrap;
  EXPECT_THROW(
    unwrap(capa::Result<void>(capa::Error{capa::ErrorKind::InvalidArgument, "x"})),
    std::invalid_argument);
  EXPECT_THROW(
    unwrap(capa::Result<int>(capa::Error{capa::ErrorKind::Failed, "x"})), std::runtime_error);
  try {
    unwrap(capa::Result<void>(capa::Error{capa::ErrorKind::System, "kernel said no", EPERM}));
    ADD_FAILURE() << "no exception";
  } catch (const capa::detail::SystemFailure & e) {
    EXPECT_EQ(e.code(), EPERM);
    EXPECT_STREQ(e.what(), "kernel said no");
  }
  EXPECT_EQ(unwrap(capa::Result<int>(7)), 7);
}
