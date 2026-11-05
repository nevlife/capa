#include "capa/spec.hpp"
#include "result_test_util.hpp"

#include <gtest/gtest.h>
#include <sched.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

// Which thread runs a process's signal handler. The kernel offers a process-directed signal to
// the thread it suggests first (complete_signal() in kernel/signal.c): the first thread for kill()
// and a terminal, the forking thread for SIGCHLD. Another thread takes it only when that one does
// not want it (wants_signal(): blocked, exiting, stopped, or asleep with a signal pending).

namespace fs = std::filesystem;

namespace
{

std::atomic<int> taker{0};

void record_taker(int)
{
  taker = static_cast<int>(::syscall(SYS_gettid));
}

auto gettid_now() -> int
{
  return static_cast<int>(::syscall(SYS_gettid));
}

auto wait_for_taker() -> int
{
  for (int i = 0; i < 2000 && taker == 0; ++i) {
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
  return taker;
}

class Handler
{
public:
  explicit Handler(int sig) : sig_(sig)
  {
    struct sigaction sa = {};
    sa.sa_handler = record_taker;
    sigemptyset(&sa.sa_mask);
    ::sigaction(sig_, &sa, &old_);
  }
  Handler(const Handler &) = delete;
  auto operator=(const Handler &) -> Handler & = delete;
  ~Handler() { ::sigaction(sig_, &old_, nullptr); }

private:
  int sig_;
  struct sigaction old_ = {};
};

void block(int sig, int how)
{
  sigset_t s;
  sigemptyset(&s);
  sigaddset(&s, sig);
  ::pthread_sigmask(how, &s, nullptr);
}

// A thread that blocks nothing and sleeps until stopped: the RT stage thread of the question.
class Bystander
{
public:
  Bystander()
  : thread_([this] {
      tid_ = gettid_now();
      while (!stop_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    })
  {
    while (tid_ == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  Bystander(const Bystander &) = delete;
  auto operator=(const Bystander &) -> Bystander & = delete;
  ~Bystander()
  {
    stop_ = true;
    thread_.join();
  }
  [[nodiscard]] auto tid() const -> int { return tid_; }

private:
  std::atomic<bool> stop_{false};
  std::atomic<int> tid_{0};
  std::thread thread_;
};

// The signal is sent from a third thread that blocks it, so neither the sender nor a running
// first thread is what decides.
auto kill_from_another_thread(int sig) -> int
{
  taker = 0;
  std::thread sender([sig] {
    block(sig, SIG_BLOCK);
    ::kill(::getpid(), sig);
  });
  sender.join();
  return wait_for_taker();
}

}  // namespace

TEST(Kernel, AProcessSignalGoesToTheFirstThreadFirst)
{
  ASSERT_EQ(gettid_now(), ::getpid()) << "gtest runs the test on the first thread";
  Handler h(SIGUSR1);
  Bystander stage;
  for (int i = 0; i < 50; ++i) {
    EXPECT_EQ(kill_from_another_thread(SIGUSR1), ::getpid());
  }

  block(SIGUSR1, SIG_BLOCK);
  for (int i = 0; i < 50; ++i) {
    const int t = kill_from_another_thread(SIGUSR1);
    EXPECT_NE(t, 0);
    EXPECT_NE(t, ::getpid()) << "the first thread blocks it, so another thread takes it";
  }
  block(SIGUSR1, SIG_UNBLOCK);
}

TEST(Kernel, SigchldGoesToTheThreadThatForkedTheChild)
{
  Handler h(SIGCHLD);
  Bystander stage;
  for (int i = 0; i < 20; ++i) {
    taker = 0;
    std::atomic<int> forker{0};
    std::thread f([&] {
      forker = gettid_now();
      const pid_t child = ::fork();
      if (child == 0) {
        ::_exit(0);
      }
      wait_for_taker();
      ::waitpid(child, nullptr, 0);
    });
    f.join();
    EXPECT_EQ(taker.load(), forker.load());
  }
}

namespace
{

// A /proc snapshot of one process: its first thread and an RT thread, each with a status file.
class Proc
{
public:
  static constexpr int kPid = 4000;
  static constexpr int kTid = 4007;

  Proc()
  {
    std::string tmpl = (fs::temp_directory_path() / "capa-signals-XXXXXX").string();
    root_ = ::mkdtemp(tmpl.data());
  }
  Proc(const Proc &) = delete;
  auto operator=(const Proc &) -> Proc & = delete;
  ~Proc() { fs::remove_all(root_); }

  // `caught` and `blocked` are SigCgt and SigBlk masks; bit n-1 is signal n.
  void thread(int tid, unsigned long caught, unsigned long blocked, char state = 'S') const
  {
    for (const std::string & dir :
         {"/proc/" + std::to_string(tid),
          "/proc/" + std::to_string(kPid) + "/task/" + std::to_string(tid)}) {
      fs::create_directories(root_ + dir);
      std::ofstream(root_ + dir + "/status")
        << "State:\t" << state << " (x)\nTgid:\t" << kPid << "\nSigBlk:\t" << std::hex << blocked
        << "\nSigCgt:\t" << caught << std::dec << "\n";
    }
  }

  [[nodiscard]] auto signals(int tid, int policy = SCHED_FIFO) const -> capa::Finding
  {
    capa::ThreadState st;
    st.policy = policy;
    st.priority = policy == SCHED_FIFO ? 50 : 0;
    const capa::Report rep = capa::detail::verify_now_at(root_, tid, st);
    const capa::Finding * f = rep.find("signals");
    return f != nullptr ? *f : capa::Finding{};
  }

private:
  std::string root_;
};

constexpr unsigned long kSigint = 1UL << 1;
constexpr unsigned long kSigterm = 1UL << 14;
constexpr unsigned long kSigchld = 1UL << 16;

}  // namespace

TEST(Verify, SignalsAreJudgedByTheThreadTheKernelOffersThemFirst)
{
  Proc p;
  const unsigned long caught = kSigint | kSigterm;

  p.thread(Proc::kPid, caught, 0);
  p.thread(Proc::kTid, caught, 0);
  const auto info = p.signals(Proc::kTid);
  EXPECT_EQ(info.verdict, capa::Verdict::Info) << info.detail;
  EXPECT_NE(info.detail.find("first thread (tid 4000)"), std::string::npos) << info.detail;

  EXPECT_EQ(p.signals(Proc::kPid).verdict, capa::Verdict::Warn) << "the RT thread is the first";

  p.thread(Proc::kPid, caught, kSigterm);
  const auto blocked = p.signals(Proc::kTid);
  EXPECT_EQ(blocked.verdict, capa::Verdict::Warn) << blocked.detail;
  EXPECT_NE(blocked.detail.find("SIGTERM"), std::string::npos) << blocked.detail;
  EXPECT_EQ(blocked.detail.find("SIGINT"), std::string::npos) << blocked.detail;

  p.thread(Proc::kPid, caught, 0, 'Z');
  EXPECT_EQ(p.signals(Proc::kTid).verdict, capa::Verdict::Warn) << "an exited first thread";

  p.thread(Proc::kPid, caught | kSigchld, 0);
  p.thread(Proc::kTid, caught | kSigchld, 0);
  const auto chld = p.signals(Proc::kTid);
  EXPECT_EQ(chld.verdict, capa::Verdict::Warn) << "SIGCHLD goes to the forking thread";
  EXPECT_NE(chld.detail.find("SIGCHLD"), std::string::npos) << chld.detail;

  p.thread(Proc::kTid, caught | kSigchld, caught | kSigchld);
  EXPECT_EQ(p.signals(Proc::kTid).verdict, capa::Verdict::Ok);

  EXPECT_EQ(p.signals(Proc::kTid, SCHED_OTHER).verdict, capa::Verdict::Unknown);
}
