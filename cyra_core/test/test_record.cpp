#include "cyra/spec.hpp"

#include <dirent.h>
#include <gtest/gtest.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>

// Other processes see which thread holds which stage through a per-process record, and tell a
// live record from a dead one by its lock. These tests run holders in forked children, since the
// point is what one process can see of another.

namespace
{

std::string run_dir()
{
  const char * d = std::getenv("CYRA_RUN_DIR");
  return d != nullptr ? d : "/dev/shm";
}

std::string spec_file()
{
  const std::string path = "/tmp/cyra_record_" + std::to_string(::getpid()) + ".yaml";
  std::ofstream(path) << "chains:\n  c:\n    target: soft\n    stages:\n"
                         "      - {node: /rec, group: work, cpus: [0]}\n"
                         "      - {name: helper, cpus: [0]}\n";
  return path;
}

// A child that binds `group` of /rec, reports its tid through a pipe, and waits to be killed.
pid_t holder(const std::string & spec, const std::string & group, int & tid)
{
  int fds[2];
  if (::pipe(fds) != 0) std::abort();
  const pid_t pid = ::fork();
  if (pid == 0) {
    ::close(fds[0]);
    ::setenv("CYRA_SPEC", spec.c_str(), 1);
    cyra::bind("/rec", group);
    const int me = cyra::this_tid();
    if (::write(fds[1], &me, sizeof me) != sizeof me) ::_exit(1);
    for (;;) ::pause();
  }
  ::close(fds[1]);
  if (::read(fds[0], &tid, sizeof tid) != sizeof tid) tid = 0;
  ::close(fds[0]);
  return pid;
}

bool bound(pid_t pid, int tid)
{
  for (const cyra::Binding & b : cyra::bindings()) {
    if (b.pid == pid && b.tid == tid && b.node == "/rec" && b.group == "work") return true;
  }
  return false;
}

bool record_file_exists(pid_t pid)
{
  DIR * d = ::opendir(run_dir().c_str());
  bool found = false;
  const std::string prefix = "cyra." + std::to_string(pid) + ".";
  while (const dirent * e = ::readdir(d)) {
    if (std::string(e->d_name).rfind(prefix, 0) == 0) found = true;
  }
  ::closedir(d);
  return found;
}

void reap(pid_t pid)
{
  ::kill(pid, SIGKILL);
  ::waitpid(pid, nullptr, 0);
}

}  // namespace

TEST(Record, AnotherProcessSeesTheStageAThreadHolds)
{
  const std::string spec = spec_file();
  int tid = 0;
  const pid_t pid = holder(spec, "work", tid);
  ASSERT_GT(tid, 0);

  EXPECT_TRUE(bound(pid, tid)) << "the holder's stage is not in bindings()";
  const cyra::Report rep = cyra::verify_process(cyra::Spec::load(spec), pid);
  EXPECT_TRUE(rep.ok()) << rep.to_string();
  EXPECT_EQ(rep.find("record"), nullptr) << "the holder has a record:\n" << rep.to_string();

  reap(pid);
  EXPECT_FALSE(bound(pid, tid)) << "a dead process still reads as holding its stage";
  std::remove(spec.c_str());
}

// Reading changes nothing: a record left by a dead process stays until a writer sweeps it.
TEST(Record, ReadingLeavesADeadRecordInPlace)
{
  const std::string spec = spec_file();
  int tid = 0;
  const pid_t pid = holder(spec, "work", tid);
  reap(pid);
  cyra::bindings();
  EXPECT_TRUE(record_file_exists(pid)) << "bindings() deleted what it was reading";
  std::remove(spec.c_str());
}

// OFD locks follow the open file description, which fork shares. A child that outlives its
// parent must not keep the parent's record alive.
TEST(Record, AForkedChildDoesNotKeepItsParentAlive)
{
  const std::string spec = spec_file();
  int fds[2];
  ASSERT_EQ(::pipe(fds), 0);
  const pid_t parent = ::fork();
  if (parent == 0) {
    ::close(fds[0]);
    ::setenv("CYRA_SPEC", spec.c_str(), 1);
    cyra::bind("/rec", "work");
    const pid_t child = ::fork();
    if (child == 0) {
      for (;;) ::pause();
    }
    if (::write(fds[1], &child, sizeof child) != sizeof child) ::_exit(1);
    ::_exit(0);
  }
  ::close(fds[1]);
  pid_t child = 0;
  ASSERT_EQ(::read(fds[0], &child, sizeof child), static_cast<ssize_t>(sizeof child));
  ::close(fds[0]);
  ::waitpid(parent, nullptr, 0);

  for (const cyra::Binding & b : cyra::bindings()) {
    EXPECT_NE(b.pid, parent) << "the exited parent reads as live through its forked child";
  }
  ::kill(child, SIGKILL);
  std::remove(spec.c_str());
}

TEST(Record, AProcessThatIsGoneIsAFailure)
{
  const std::string spec = spec_file();
  int tid = 0;
  const pid_t pid = holder(spec, "work", tid);
  reap(pid);
  const cyra::Report rep = cyra::verify_process(cyra::Spec::load(spec), pid);
  ASSERT_NE(rep.find("observed-process"), nullptr) << rep.to_string();
  EXPECT_EQ(rep.find("observed-process")->verdict, cyra::Verdict::Fail);
  EXPECT_THROW(cyra::verify_process(cyra::Spec::load(spec), 0), std::invalid_argument);
  std::remove(spec.c_str());
}

// A thread that is applied but not recorded is invisible to every other process. That is said in
// the report rather than left for the CLI to show as an unbound stage.
TEST(Record, AnUnwritableRecordIsReportedNotHidden)
{
  const std::string spec = spec_file();
  int fds[2];
  ASSERT_EQ(::pipe(fds), 0);
  const pid_t pid = ::fork();
  if (pid == 0) {
    ::close(fds[0]);
    ::setenv("CYRA_RUN_DIR", "/nonexistent/cyra", 1);
    ::setenv("CYRA_SPEC", spec.c_str(), 1);
    const cyra::Report rep = cyra::bind("/rec", "work");
    const cyra::Finding * f = rep.find("record");
    const char verdict = f == nullptr ? 'n' : f->verdict == cyra::Verdict::Unknown ? 'u' : 'x';
    if (::write(fds[1], &verdict, 1) != 1) ::_exit(1);
    ::_exit(0);
  }
  ::close(fds[1]);
  char verdict = 0;
  ASSERT_EQ(::read(fds[0], &verdict, 1), 1);
  ::close(fds[0]);
  ::waitpid(pid, nullptr, 0);
  EXPECT_EQ(verdict, 'u') << "n: no record finding, x: not Unknown";
  std::remove(spec.c_str());
}
