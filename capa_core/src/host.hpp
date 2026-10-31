#ifndef CAPA_HOST_HPP
#define CAPA_HOST_HPP

#include "capa/spec.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Reading the host. Every path goes through Fs, so a test can hand preflight a snapshot of
// another machine's /proc and /sys.

namespace capa::host
{

class Fs
{
public:
  explicit Fs(std::string root = {}) : root_(std::move(root)) {}

  [[nodiscard]] auto read(const std::string & path) const -> std::optional<std::string>;
  [[nodiscard]] auto list(const std::string & dir) const -> std::vector<std::string>;
  [[nodiscard]] auto exists(const std::string & path) const -> bool;
  [[nodiscard]] auto path(const std::string & p) const -> std::string { return root_ + p; }

private:
  std::string root_;
};

using Cpus = std::vector<std::uint32_t>;

auto trimmed(std::string s) -> std::string;
auto parse_cpu_list(const std::string & text) -> Cpus;
auto parse_cpu_mask(const std::string & text) -> Cpus;
auto contains(const Cpus & set, std::uint32_t cpu) -> bool;
auto overlap(const Cpus & a, const Cpus & b) -> bool;
auto join(const Cpus & cpus) -> std::string;
auto field(const std::string & text, const std::string & key) -> std::optional<std::string>;
auto boot_param(const std::string & cmdline, const std::string & key) -> std::optional<std::string>;
auto words(const std::string & text) -> std::vector<std::string>;
auto list_some(const std::vector<std::string> & items, std::size_t most) -> std::string;

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

auto task(const Fs & fs, int pid, int tid) -> std::optional<Task>;
auto tasks(const Fs & fs, bool every_thread) -> std::vector<Task>;

// The cgroup v2 directory of `pid` under /sys/fs/cgroup, or empty without a v2 line.
auto cgroup_dir(const Fs & fs, int pid) -> std::string;
auto cgroup_chain(const std::string & dir) -> std::vector<std::string>;

auto isolated_cpus(const Fs & fs) -> Cpus;

// Seccomp, LSM label and hidepid of this process: what can refuse an RT request besides
// permission. Empty when none applies.
auto blocking_layers(const Fs & fs) -> std::string;

void preflight_into(
  Report & rep, const Fs & fs, int tid, const Options & opts, int control, Strictness strict);
auto preflight_spec(const Fs & fs, const Spec & spec) -> Report;

void verify_now(Report & rep, const Fs & fs, int tid, const ThreadState & st);
void verify_window(Report & rep, const Stage & stage, int tid, std::chrono::milliseconds window);

}  // namespace capa::host

#endif  // CAPA_HOST_HPP
