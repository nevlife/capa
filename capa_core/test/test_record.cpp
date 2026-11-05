#include "capa/spec.hpp"
#include "result_test_util.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

// Other processes see which thread holds which stage through a per-process record, and tell a
// live record from a dead one by its lock. These tests run holders in forked children, since the
// point is what one process can see of another.

namespace
{

auto run_dir() -> std::string
{
  const char * d = std::getenv("CAPA_RUN_DIR");
  return d != nullptr ? d : "/dev/shm";
}

auto spec_file() -> std::string
{
  const std::string path = "/tmp/capa_record_" + std::to_string(::getpid()) + ".yaml";
  std::ofstream(path) << "chains:\n  c:\n    target: soft\n    stages:\n"
                         "      - {node: /rec, group: work, cpus: [0]}\n"
                         "      - {name: helper, cpus: [0]}\n";
  return path;
}

// A child that binds `group` of /rec, reports its tid through a pipe, and waits to be killed.
auto holder(const std::string & spec, const std::string & group, int & tid) -> pid_t
{
  int fds[2];
  if (::pipe(fds) != 0) {
    std::abort();
  }
  const pid_t pid = ::fork();
  if (pid == 0) {
    ::close(fds[0]);
    ::setenv("CAPA_SPEC", spec.c_str(), 1);
    must(capa::bind("/rec", group));
    const int me = capa::this_tid();
    if (::write(fds[1], &me, sizeof me) != sizeof me) {
      ::_exit(1);
    }
    for (;;) {
      ::pause();
    }
  }
  ::close(fds[1]);
  if (::read(fds[0], &tid, sizeof tid) != sizeof tid) {
    tid = 0;
  }
  ::close(fds[0]);
  return pid;
}

auto bound(pid_t pid, int tid) -> bool
{
  for (const capa::Binding & b : capa::bindings()) {
    if (b.pid == pid && b.tid == tid && b.node == "/rec" && b.group == "work") {
      return true;
    }
  }
  return false;
}

auto record_file_exists(pid_t pid) -> bool
{
  DIR * d = ::opendir(run_dir().c_str());
  bool found = false;
  const std::string prefix = "capa." + std::to_string(pid) + ".";
  while (const dirent * e = ::readdir(d)) {
    if (std::string(e->d_name).rfind(prefix, 0) == 0) {
      found = true;
    }
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
  const capa::Report rep = must(capa::verify_process(must(capa::Spec::load(spec)), pid));
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
  capa::bindings();
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
    ::setenv("CAPA_SPEC", spec.c_str(), 1);
    must(capa::bind("/rec", "work"));
    const pid_t child = ::fork();
    if (child == 0) {
      for (;;) {
        ::pause();
      }
    }
    if (::write(fds[1], &child, sizeof child) != sizeof child) {
      ::_exit(1);
    }
    ::_exit(0);
  }
  ::close(fds[1]);
  pid_t child = 0;
  ASSERT_EQ(::read(fds[0], &child, sizeof child), static_cast<ssize_t>(sizeof child));
  ::close(fds[0]);
  ::waitpid(parent, nullptr, 0);

  for (const capa::Binding & b : capa::bindings()) {
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
  const capa::Report rep = must(capa::verify_process(must(capa::Spec::load(spec)), pid));
  ASSERT_NE(rep.find("observed-process"), nullptr) << rep.to_string();
  EXPECT_EQ(rep.find("observed-process")->verdict, capa::Verdict::Fail);
  EXPECT_EQ(
    kind_of(capa::verify_process(must(capa::Spec::load(spec)), 0)),
    capa::ErrorKind::InvalidArgument);
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
    ::setenv("CAPA_RUN_DIR", "/nonexistent/capa", 1);
    ::setenv("CAPA_SPEC", spec.c_str(), 1);
    const capa::Report rep = must(capa::bind("/rec", "work"));
    const capa::Finding * f = rep.find("record");
    const char verdict = f == nullptr ? 'n' : f->verdict == capa::Verdict::Unknown ? 'u' : 'x';
    if (::write(fds[1], &verdict, 1) != 1) {
      ::_exit(1);
    }
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

// ---- what a reader believes ----

namespace
{

auto fnv1a(const std::string & s) -> std::uint64_t
{
  std::uint64_t h = 14695981039346656037ULL;
  for (const unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ULL;
  }
  return h;
}

auto framed(const std::string & body, bool whole = true) -> std::string
{
  char sum[17];
  const std::uint64_t h = fnv1a(body) + (whole ? 0 : 1);
  std::snprintf(sum, sizeof sum, "%016" PRIx64, h);
  return "capa-record 3 " + std::to_string(body.size()) + " " + sum + "\n" + body;
}

auto line(const std::string & node, int pid, int tid) -> std::string
{
  return node + "\t\t\t" + std::to_string(pid) + "\t" + std::to_string(tid) + "\t" +
         std::to_string(capa::detail::task_starttime(pid, tid)) + "\t-\n";
}

// A child that holds a record it wrote itself, locked, under its own name. `make` builds the text
// from the child's pid. Returns the child's pid once the record is in place.
template <typename F>
auto writer(F make) -> pid_t
{
  int fds[2];
  if (::pipe(fds) != 0) {
    std::abort();
  }
  const pid_t pid = ::fork();
  if (pid == 0) {
    ::close(fds[0]);
    const pid_t me = ::getpid();
    const std::string path = run_dir() + "/capa." + std::to_string(me) + "." +
                             std::to_string(capa::detail::task_starttime(me, me));
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    ::fchmod(fd, 0644);
    struct flock fl
    {
    };
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    const std::string text = make(me);
    const bool ok = fd >= 0 && ::fcntl(fd, F_OFD_SETLK, &fl) == 0 &&
                    ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
    const char c = ok ? 'y' : 'n';
    if (::write(fds[1], &c, 1) != 1) {
      ::_exit(1);
    }
    for (;;) {
      ::pause();
    }
  }
  ::close(fds[1]);
  char c = 0;
  if (::read(fds[0], &c, 1) != 1 || c != 'y') {
    std::abort();
  }
  ::close(fds[0]);
  return pid;
}

auto seen(const std::string & node, pid_t pid) -> bool
{
  for (const capa::Binding & b : capa::bindings()) {
    if (b.node == node && b.pid == pid) {
      return true;
    }
  }
  return false;
}

}  // namespace

// Anyone can write the run directory, so a bind record is believed only about its own process.
TEST(Record, ABindRecordSpeaksOnlyForItsOwnProcess)
{
  const pid_t me = ::getpid();
  const pid_t pid =
    writer([&](pid_t self) { return framed(line("/own", self, self) + line("/foreign", me, me)); });
  EXPECT_TRUE(seen("/own", pid)) << "the writer's own line was not read";
  EXPECT_FALSE(seen("/foreign", me))
    << "a record named a thread of another process and was believed";
  reap(pid);
}

// Read in the middle of a rewrite, a record is a new head on an old tail. The length and checksum
// in its header tell that apart from a whole one.
TEST(Record, ARecordThatDoesNotAddUpIsNotRead)
{
  const pid_t whole = writer([](pid_t self) { return framed(line("/whole", self, self)); });
  const pid_t pid = writer([](pid_t self) { return framed(line("/torn", self, self), false); });
  EXPECT_TRUE(seen("/whole", whole)) << "a whole record was not read";
  EXPECT_FALSE(seen("/torn", pid));
  reap(whole);
  reap(pid);
}

TEST(Record, AFifoUnderARecordNameDoesNotStallAReader)
{
  const std::string path = run_dir() + "/capa.4194000.1";
  ASSERT_EQ(::mkfifo(path.c_str(), 0600), 0) << std::strerror(errno);
  const pid_t pid = ::fork();
  if (pid == 0) {
    capa::bindings();
    ::_exit(0);
  }
  bool done = false;
  for (int i = 0; i < 500 && !done; ++i) {
    done = ::waitpid(pid, nullptr, WNOHANG) == pid;
    if (!done) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  if (!done) {
    reap(pid);
  }
  std::remove(path.c_str());
  EXPECT_TRUE(done) << "bindings() blocked opening a fifo";
}

// The daemon names threads of other processes. Its directory vouches for that, so a directory
// others could write to vouches for nothing: not written to, and not read from.
TEST(Record, TheDaemonsRecordIsBelievedOnlyFromADirectoryNoOneElseCanWrite)
{
  const std::string dir = run_dir() + "/daemon." + std::to_string(::getpid());
  ::setenv("CAPA_DAEMON_DIR", dir.c_str(), 1);
  const pid_t me = ::getpid();
  int fds[2];
  ASSERT_EQ(::pipe(fds), 0);
  const pid_t pid = ::fork();
  if (pid == 0) {
    ::close(fds[0]);
    const std::string why = capa::detail::record_bindings({{"/held", "", "", me, me}});
    const char c = why.empty() ? 'y' : 'n';
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
  EXPECT_EQ(c, 'y') << "the daemon could not write its record";
  EXPECT_TRUE(seen("/held", me));

  ASSERT_EQ(::chmod(dir.c_str(), 0777), 0);
  EXPECT_FALSE(seen("/held", me)) << "a record in a directory anyone can write was believed";
  EXPECT_FALSE(capa::detail::record_bindings({}).empty())
    << "the daemon wrote into a directory anyone can write";

  reap(pid);
  ::chmod(dir.c_str(), 0755);
  std::filesystem::remove_all(dir);
  ::unsetenv("CAPA_DAEMON_DIR");
}

// The kernel does not say which thread started which, so a bind records the threads that were
// already there. Those are never the stage's children.
TEST(Record, ABindRecordsTheThreadsThatCameBeforeIt)
{
  const std::string spec = spec_file();
  int fds[2];
  ASSERT_EQ(::pipe(fds), 0);
  const pid_t pid = ::fork();
  if (pid == 0) {
    ::close(fds[0]);
    std::atomic<int> before{0};
    std::atomic<int> after{0};
    std::thread b([&] {
      before.store(capa::this_tid());
      for (;;) {
        ::pause();
      }
    });
    while (before.load() == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ::setenv("CAPA_SPEC", spec.c_str(), 1);
    must(capa::bind("/rec", "work"));
    std::thread a([&] {
      after.store(capa::this_tid());
      for (;;) {
        ::pause();
      }
    });
    while (after.load() == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const int both[2] = {before.load(), after.load()};
    if (::write(fds[1], both, sizeof both) != sizeof both) {
      ::_exit(1);
    }
    for (;;) {
      ::pause();
    }
  }
  ::close(fds[1]);
  int both[2] = {0, 0};
  ASSERT_EQ(::read(fds[0], both, sizeof both), static_cast<ssize_t>(sizeof both));
  ::close(fds[0]);

  bool found = false;
  for (const auto & h : capa::detail::held_stages()) {
    if (h.binding.pid != pid) {
      continue;
    }
    found = true;
    auto has = [&](int tid) {
      for (const auto & [t, start] : h.earlier) {
        if (t == tid) {
          return true;
        }
      }
      return false;
    };
    EXPECT_TRUE(has(both[0])) << "a thread from before the bind is not listed";
    EXPECT_FALSE(has(both[1])) << "a thread started after the bind is listed as earlier";
    EXPECT_FALSE(has(h.binding.tid)) << "the holder lists itself";
  }
  EXPECT_TRUE(found);
  reap(pid);
  std::remove(spec.c_str());
}

// A process whose only held thread was set by the daemon has no record of its own, and needs
// none: the daemon's record names its thread.
TEST(Record, AThreadTheDaemonSetCountsAsRecorded)
{
  const std::string spec = spec_file();
  const std::string dir = run_dir() + "/daemon_v." + std::to_string(::getpid());
  ::setenv("CAPA_DAEMON_DIR", dir.c_str(), 1);
  const pid_t target = ::fork();
  if (target == 0) {
    for (;;) {
      ::pause();
    }
  }
  int fds[2];
  ASSERT_EQ(::pipe(fds), 0);
  const pid_t daemon = ::fork();
  if (daemon == 0) {
    ::close(fds[0]);
    const std::string why = capa::detail::record_bindings({{"", "", "helper", target, target}});
    const char c = why.empty() ? 'y' : 'n';
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
  ASSERT_EQ(c, 'y') << "the daemon could not write its record";

  const capa::Report rep = must(capa::verify_process(must(capa::Spec::load(spec)), target));
  EXPECT_EQ(rep.find("record"), nullptr) << rep.to_string();

  reap(daemon);
  const capa::Report after = must(capa::verify_process(must(capa::Spec::load(spec)), target));
  EXPECT_NE(after.find("record"), nullptr) << "with no record naming it, the holders are unknown";

  reap(target);
  std::filesystem::remove_all(dir);
  ::unsetenv("CAPA_DAEMON_DIR");
  std::remove(spec.c_str());
}
