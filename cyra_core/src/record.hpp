#ifndef CYRA_RECORD_HPP
#define CYRA_RECORD_HPP

#include <cstdint>
#include <string>
#include <vector>

// Which thread of this process holds which stage, published for other processes to read (the CLI,
// the daemon). One file per process, write-locked with an OFD lock for as long as the process
// lives: a reader tells a live record from a dead one by that lock, and the kernel drops it when
// the process exits. Internal; the public face is cyra::bindings().

namespace cyra::record
{

struct Entry
{
  std::string node;
  std::string group;
  std::string name;
  int pid = 0;  // the thread's process: this one for a bind, another for the daemon's stages
  int tid = 0;
  std::uint64_t tid_start = 0;
};

struct Process
{
  int pid = 0;
  std::vector<Entry> entries;
};

// Rewrite this process's record with `entries`. Creates it (and sweeps dead records) on the first
// call. Returns an empty string, or why the record could not be written.
std::string publish(const std::vector<Entry> & entries);

// In a forked child: the record belongs to the parent, so drop it without touching the file.
void forget_after_fork() noexcept;

// Every live record on this host. Entries whose thread is gone or recycled are left out.
std::vector<Process> read_live();

// /proc/<pid>/task/<tid>/stat field 22, 0 when unreadable.
std::uint64_t task_starttime(int pid, int tid);

}  // namespace cyra::record

#endif  // CYRA_RECORD_HPP
