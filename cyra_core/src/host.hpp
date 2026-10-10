#ifndef CYRA_HOST_HPP
#define CYRA_HOST_HPP

#include "cyra/spec.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Reading the host. Every path goes through Fs, so a test can hand preflight a snapshot of
// another machine's /proc and /sys.

namespace cyra::host
{

class Fs
{
public:
  explicit Fs(std::string root = {}) : root_(std::move(root)) {}

  std::optional<std::string> read(const std::string & path) const;
  std::vector<std::string> list(const std::string & dir) const;
  bool exists(const std::string & path) const;
  std::string path(const std::string & p) const { return root_ + p; }

private:
  std::string root_;
};

using Cpus = std::vector<std::uint32_t>;

std::string trimmed(std::string s);
Cpus parse_cpu_list(const std::string & text);
Cpus parse_cpu_mask(const std::string & text);
bool contains(const Cpus & set, std::uint32_t cpu);
bool overlap(const Cpus & a, const Cpus & b);
std::string join(const Cpus & cpus);
std::optional<std::string> field(const std::string & text, const std::string & key);
std::optional<std::string> boot_param(const std::string & cmdline, const std::string & key);
std::vector<std::string> words(const std::string & text);
std::string list_some(const std::vector<std::string> & items, std::size_t most);

constexpr int kSchedDeadline = 6;

struct Task
{
  int pid = 0;
  int tid = 0;
  std::string comm;
  int policy = 0;
  int rt_priority = 0;
  int prio = 0;  // field 18: the effective priority, PI boost included
  std::uint64_t minflt = 0;
  std::uint64_t majflt = 0;
  bool kernel = false;
  Cpus cpus;
};

std::optional<Task> task(const Fs & fs, int pid, int tid);
std::vector<Task> tasks(const Fs & fs, bool every_thread);

// The cgroup v2 directory of `pid` under /sys/fs/cgroup, or empty without a v2 line.
std::string cgroup_dir(const Fs & fs, int pid);
std::vector<std::string> cgroup_chain(const std::string & dir);

Cpus isolated_cpus(const Fs & fs);

// Seccomp, LSM label and hidepid of this process: what can refuse an RT request besides
// permission. Empty when none applies.
std::string blocking_layers(const Fs & fs);

void preflight_into(Report & rep, const Fs & fs, int tid, const Options & opts, int control);
Report preflight_spec(const Fs & fs, const Spec & spec);

void verify_now(Report & rep, int tid, const ThreadState & st);
void verify_window(Report & rep, const Stage & stage, int tid, std::chrono::milliseconds window);

}  // namespace cyra::host

#endif  // CYRA_HOST_HPP
