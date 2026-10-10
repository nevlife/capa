#include "cyra/spec.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

// preflight judged on snapshots of other machines (test/hosts/*.host, written by capture.sh or by
// hand): "=== <path>" starts a file, "=== <dir>/" makes a directory.

namespace fs = std::filesystem;
using cyra::Verdict;

namespace
{

class Host
{
public:
  explicit Host(const std::string & name)
  {
    std::string tmpl = (fs::temp_directory_path() / "cyra-host-XXXXXX").string();
    root_ = ::mkdtemp(tmpl.data());
    std::ifstream in(std::string(CYRA_HOSTS_DIR) + "/" + name + ".host");
    std::string line;
    std::ofstream out;
    while (std::getline(in, line)) {
      if (line.rfind("=== ", 0) != 0) {
        out << line << '\n';
        continue;
      }
      out.close();
      const fs::path p = root_ + line.substr(4);
      if (line.back() == '/') {
        fs::create_directories(p);
        continue;
      }
      fs::create_directories(p.parent_path());
      out.open(p);
    }
  }
  Host(const Host &) = delete;
  Host & operator=(const Host &) = delete;
  ~Host() { fs::remove_all(root_); }

  cyra::Report preflight(const cyra::Options & opts, int control = 0) const
  {
    return cyra::detail::preflight_at(root_, 0, opts, control);
  }

  cyra::Report preflight(const std::string & yaml) const
  {
    const std::string file = root_ + "/chain.yaml";
    std::ofstream(file) << yaml;
    return cyra::detail::preflight_at(root_, cyra::Spec::load(file));
  }

private:
  std::string root_;
};

cyra::Options fifo(int priority, std::vector<std::uint32_t> cpus)
{
  cyra::Options o;
  o.policy = cyra::Policy::Fifo;
  o.priority = priority;
  o.cpus = std::move(cpus);
  return o;
}

void expect(const cyra::Report & rep, const std::map<std::string, Verdict> & want)
{
  for (const auto & [id, v] : want) {
    const auto * f = rep.find(id.c_str());
    ASSERT_NE(f, nullptr) << id << " missing\n" << rep.to_string();
    EXPECT_EQ(f->verdict, v) << id << ": " << f->detail;
  }
}

}  // namespace

TEST(Hosts, CapturedX86Desktop)
{
  const Host host("x86_desktop");
  const auto rep = host.preflight(fifo(80, {2}), 90);
  expect(
    rep, {
           {"policy-permission", Verdict::Fail},
           {"rt-throttle", Verdict::Warn},
           {"cpu-isolation", Verdict::Warn},
           {"cpuset", Verdict::Ok},
           {"cpu-governor", Verdict::Warn},
           {"cpufreq", Verdict::Warn},
           {"rcu-offload", Verdict::Warn},
           {"irq-managed", Verdict::Unknown},
           {"irq-default-affinity", Verdict::Warn},
           {"cpu-idle", Verdict::Warn},
           {"workqueue", Verdict::Warn},
           {"kthread-affinity", Verdict::Warn},
           {"smt", Verdict::Warn},
           {"softlockup-watchdog", Verdict::Warn},
           {"kernel-preemption", Verdict::Warn},
           {"preempt-mode", Verdict::Unknown},
           {"sched-ext", Verdict::Ok},
           {"thp", Verdict::Ok},
           {"compaction", Verdict::Warn},
           {"numa-balancing", Verdict::Ok},
           {"dirty-limits", Verdict::Warn},
           {"clocksource", Verdict::Ok},
           {"virtualization", Verdict::Ok},
           {"power-policy", Verdict::Warn},
           {"memory-lock", Verdict::Warn},
           {"secure-exec", Verdict::Unknown},
           {"bind-now", Verdict::Unknown},
         });
  EXPECT_NE(rep.find("rt-throttle")->detail.find("debugfs"), std::string::npos)
    << "a 6.12+ kernel whose fair server cannot be read must say so";
}

TEST(Hosts, OrinJetPack6)
{
  const Host host("orin_jetpack6");
  expect(
    host.preflight(fifo(80, {4, 5}), 90), {
                                            {"policy-permission", Verdict::Ok},
                                            {"rt-throttle", Verdict::Ok},
                                            {"cpu-isolation", Verdict::Ok},
                                            {"cpuset", Verdict::Ok},
                                            {"cpu-governor", Verdict::Warn},
                                            {"cpufreq", Verdict::Ok},
                                            {"rcu-offload", Verdict::Ok},
                                            {"irq-affinity", Verdict::Warn},
                                            {"irq-default-affinity", Verdict::Ok},
                                            {"cpu-idle", Verdict::Warn},
                                            {"workqueue", Verdict::Ok},
                                            {"kthread-affinity", Verdict::Warn},
                                            {"smt", Verdict::Ok},
                                            {"cpu-capacity", Verdict::Ok},
                                            {"softlockup-watchdog", Verdict::Ok},
                                            {"kernel-preemption", Verdict::Warn},
                                            {"preempt-mode", Verdict::Ok},
                                            {"sched-ext", Verdict::Ok},
                                            {"compaction", Verdict::Warn},
                                            {"console", Verdict::Warn},
                                            {"clocksource", Verdict::Ok},
                                            {"power-policy", Verdict::Warn},
                                            {"cgroup-cpu", Verdict::Ok},
                                            {"audit", Verdict::Unknown},
                                          });
}

TEST(Hosts, OrinJetPack6WholeFile)
{
  const Host host("orin_jetpack6");
  const auto rep = host.preflight(R"(
chains:
  camera_to_control:
    target: hard
    stages:
      - {name: eth_irq, kind: release, policy: fifo, priority: 85, cpus: [5], match: {irq: 2490000.ethernet}}
      - {name: grab, policy: fifo, priority: 70, cpus: [4]}
      - {name: control, policy: fifo, priority: 80, cpus: [8]}
)");
  expect(
    rep, {
           {"housekeeping", Verdict::Ok},
           {"foreign-rt-thread", Verdict::Warn},
           {"deadline-task", Verdict::Warn},
           {"nohz-single-task", Verdict::Ok},
           {"llc-handoff", Verdict::Ok},
           {"taint", Verdict::Ok},
         });
  const std::string foreign = rep.find("foreign-rt-thread")->detail;
  EXPECT_NE(foreign.find("irq/77-host1x"), std::string::npos) << foreign;
  EXPECT_EQ(foreign.find("irq/200"), std::string::npos) << "a matched IRQ thread is declared";
  EXPECT_NE(rep.find("deadline-task")->detail.find("sugov:4"), std::string::npos);
  EXPECT_NE(rep.find("llc-handoff")->detail.find("grab -> control"), std::string::npos);
}

TEST(Hosts, Container)
{
  const Host host("container");
  const auto rep = host.preflight(fifo(50, {6}), 90);
  expect(
    rep, {
           {"policy-permission", Verdict::Fail},
           {"cpuset", Verdict::Fail},
           {"cpu-governor", Verdict::Unknown},
           {"cpu-idle", Verdict::Unknown},
           {"preempt-mode", Verdict::Unknown},
           {"thp", Verdict::Warn},
           {"numa-balancing", Verdict::Warn},
           {"audit", Verdict::Warn},
           {"console", Verdict::Warn},
           {"clocksource", Verdict::Ok},
           {"virtualization", Verdict::Warn},
           {"cgroup-cpu", Verdict::Warn},
           {"cgroup-memory", Verdict::Warn},
         });
  const std::string why = rep.find("policy-permission")->detail;
  EXPECT_NE(why.find("seccomp mode 2"), std::string::npos) << why;
  EXPECT_NE(why.find("docker-default"), std::string::npos) << why;
}

TEST(Hosts, RtKernelReadAsRoot)
{
  const Host host("x86_rt_root");
  expect(
    host.preflight(fifo(80, {2}), 90), {
                                         {"policy-permission", Verdict::Ok},
                                         {"rt-throttle", Verdict::Warn},
                                         {"cpu-isolation", Verdict::Ok},
                                         {"cpuset", Verdict::Warn},
                                         {"cpufreq", Verdict::Ok},
                                         {"irq-managed", Verdict::Warn},
                                         {"irq-default-affinity", Verdict::Ok},
                                         {"irqbalance", Verdict::Warn},
                                         {"cpu-idle", Verdict::Ok},
                                         {"smt", Verdict::Warn},
                                         {"softlockup-watchdog", Verdict::Ok},
                                         {"kernel-preemption", Verdict::Ok},
                                         {"preempt-mode", Verdict::Ok},
                                         {"sched-ext", Verdict::Warn},
                                         {"thp", Verdict::Ok},
                                         {"compaction", Verdict::Ok},
                                         {"dirty-limits", Verdict::Ok},
                                         {"audit", Verdict::Ok},
                                         {"console", Verdict::Ok},
                                         {"clocksource", Verdict::Warn},
                                         {"memory-lock", Verdict::Ok},
                                       });
  cyra::Options inherit = fifo(80, {2});
  inherit.children = cyra::Children::Inherit;
  expect(host.preflight(inherit, 90), {{"sched-ext", Verdict::Ok}});
}

TEST(Hosts, PriorityNinetyNineIsWarned)
{
  const Host host("x86_rt_root");
  expect(host.preflight(fifo(99, {2})), {{"priority-band", Verdict::Warn}});
  expect(host.preflight(fifo(85, {2})), {{"priority-band", Verdict::Ok}});
}

TEST(Hosts, HousekeepingFailsWhenNothingIsLeft)
{
  const Host host("x86_rt_root");
  const auto rep = host.preflight(R"(
chains:
  a:
    target: hard
    stages:
      - {name: one, policy: fifo, priority: 70, cpus: [0]}
      - {name: two, policy: fifo, priority: 80, cpus: [1]}
)");
  expect(
    rep,
    {{"housekeeping", Verdict::Fail}, {"taint", Verdict::Warn}, {"smt-neighbor", Verdict::Ok}});
  EXPECT_FALSE(rep.ok());
}

TEST(Hosts, NohzFullCpuWithTwoStagesIsWarned)
{
  const Host host("x86_rt_root");
  expect(
    host.preflight(R"(
chains:
  a:
    target: soft
    stages:
      - {name: one, policy: fifo, priority: 70, cpus: [2]}
      - {name: two, policy: fifo, priority: 80, cpus: [2]}
)"),
    {{"nohz-single-task", Verdict::Warn}});
}

TEST(Hosts, SmtSiblingsHoldingTwoHardWorkersAreWarned)
{
  const Host host("x86_rt_root");
  expect(
    host.preflight(R"(
chains:
  a:
    target: hard
    stages:
      - {name: one, policy: fifo, priority: 70, cpus: [0]}
      - {name: two, policy: fifo, priority: 80, cpus: [2]}
)"),
    {{"smt-neighbor", Verdict::Warn}});
}
