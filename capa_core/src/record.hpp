#ifndef CAPA_RECORD_HPP
#define CAPA_RECORD_HPP

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// One OFD write-locked file per process; the kernel drops the lock at exit. Trust: docs/api.md 4.

namespace capa::record
{

using Task = std::pair<int, std::uint64_t>;  // tid, /proc/.../stat field 22

struct Entry
{
  std::string node;
  std::string group;
  std::string name;
  int pid = 0;  // the thread's process: the writer for a bind, another for the daemon's stages
  int tid = 0;
  std::uint64_t tid_start = 0;
  bool ask = false;  // a bind without the permission, asking the daemon to set its thread
  // Threads the process already had when the stage was bound. None of them is a child of it.
  std::vector<Task> earlier;
};

struct Process
{
  int pid = 0;
  std::vector<Entry> entries;
};

enum class Writer : std::uint8_t { Bind, Daemon };

// The first call creates the record and sweeps this user's dead ones. Returns why it failed, or "".
auto publish(Writer writer, const std::vector<Entry> & entries) -> std::string;

// In a forked child: the records are the parent's, so drop them without touching the files.
void forget_after_fork() noexcept;

// Only records a reader believes; entries whose thread is gone or recycled are left out.
auto read_live() -> std::vector<Process>;

auto tasks_of(int pid) -> std::vector<Task>;

// Created if missing. Empty, with `why` set, when it is not this user's alone.
auto daemon_dir(std::string & why) -> std::string;

auto task_starttime(int pid, int tid) -> std::uint64_t;

}  // namespace capa::record

#endif  // CAPA_RECORD_HPP
