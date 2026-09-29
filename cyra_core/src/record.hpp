#ifndef CYRA_RECORD_HPP
#define CYRA_RECORD_HPP

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Which thread holds which stage, published for other processes to read (the CLI, the daemon).
// One file per process, write-locked with an OFD lock for as long as the process lives: a reader
// tells a live record from a dead one by that lock, and the kernel drops it when the process
// exits. Internal; the public face is cyra::bindings(). What a reader believes: api.en.md 4.

namespace cyra::record
{

// A thread, told apart from a later thread that reuses its tid by its start time.
using Task = std::pair<int, std::uint64_t>;  // tid, /proc/.../stat field 22

struct Entry
{
  std::string node;
  std::string group;
  std::string name;
  int pid = 0;  // the thread's process: the writer for a bind, another for the daemon's stages
  int tid = 0;
  std::uint64_t tid_start = 0;
  // The threads the process already had when the stage was bound. None of them is a child of it.
  std::vector<Task> earlier;
};

struct Process
{
  int pid = 0;
  std::vector<Entry> entries;
};

enum class Writer : std::uint8_t { Bind, Daemon };

// Rewrite this process's record of `writer`'s kind with `entries`. Creates it (and sweeps this
// user's dead records) on the first call. Returns an empty string, or why it was not written.
std::string publish(Writer writer, const std::vector<Entry> & entries);

// In a forked child: the records belong to the parent, so drop them without touching the files.
void forget_after_fork() noexcept;

// Every live record on this host a reader believes. Entries whose thread is gone or recycled are
// left out.
std::vector<Process> read_live();

std::vector<Task> tasks_of(int pid);

// The daemon's directory, created if missing. Empty, with `why` set, when it is not this user's
// alone.
std::string daemon_dir(std::string & why);

// /proc/<pid>/task/<tid>/stat field 22, 0 when unreadable.
std::uint64_t task_starttime(int pid, int tid);

}  // namespace cyra::record

#endif  // CYRA_RECORD_HPP
