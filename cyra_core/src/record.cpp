#include "record.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <sstream>
#include <string>

namespace cyra::record
{

namespace
{

constexpr const char * kHeader = "cyra-record 2";

// /dev/shm like flux's owner files: always there, shared by every user, nothing to install.
std::string dir()
{
  const char * env = std::getenv("CYRA_RUN_DIR");
  return env != nullptr && *env != '\0' ? env : "/dev/shm";
}

std::optional<std::string> read_all(const std::string & path)
{
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return std::nullopt;
  std::string out;
  char buf[4096];
  ssize_t n;
  while ((n = ::read(fd, buf, sizeof buf)) > 0) out.append(buf, static_cast<std::size_t>(n));
  ::close(fd);
  if (n < 0) return std::nullopt;
  return out;
}

// Field 22 of a stat line. comm (field 2) may hold spaces and parens, so count from its last ')'.
std::uint64_t starttime_of(const std::string & stat_path)
{
  const auto text = read_all(stat_path);
  if (!text) return 0;
  const auto close = text->rfind(')');
  if (close == std::string::npos) return 0;
  std::istringstream rest(text->substr(close + 1));
  std::string field;
  for (int i = 3; i <= 22; ++i) {
    if (!(rest >> field)) return 0;
  }
  return std::strtoull(field.c_str(), nullptr, 10);
}

// A whole-file write lock held by any open file description is a live writer.
bool locked(const std::string & path)
{
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  struct flock fl
  {
  };
  fl.l_type = F_WRLCK;
  fl.l_whence = SEEK_SET;
  const bool held = ::fcntl(fd, F_OFD_GETLK, &fl) == 0 && fl.l_type != F_UNLCK;
  ::close(fd);
  return held;
}

// Only the final names count; a creator locks the file before renaming it into place, so a
// record under its final name is never seen unlocked while its writer lives.
bool record_name(const char * entry, int & pid)
{
  unsigned long long p = 0;
  unsigned long long start = 0;
  char tail = 0;
  if (std::sscanf(entry, "cyra.%llu.%llu%c", &p, &start, &tail) != 2) return false;
  pid = static_cast<int>(p);
  return true;
}

void sweep_dead()
{
  const std::string d = dir();
  DIR * dp = ::opendir(d.c_str());
  if (dp == nullptr) return;
  while (const dirent * e = ::readdir(dp)) {
    int pid = 0;
    if (!record_name(e->d_name, pid)) continue;
    const std::string path = d + "/" + e->d_name;
    if (!locked(path)) ::unlink(path.c_str());
  }
  ::closedir(dp);
}

int g_fd = -1;

std::string create()
{
  const std::string d = dir();
  const std::string suffix =
    std::to_string(::getpid()) + "." + std::to_string(starttime_of("/proc/self/stat"));
  const std::string tmp = d + "/.cyra.creating." + suffix;
  const std::string final = d + "/cyra." + suffix;
  const int fd = ::open(tmp.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) return "cannot create " + tmp + ": " + std::strerror(errno);
  ::fchmod(fd, 0644);  // readable by the CLI and the daemon whatever the umask
  struct flock fl
  {
  };
  fl.l_type = F_WRLCK;
  fl.l_whence = SEEK_SET;
  if (::fcntl(fd, F_OFD_SETLK, &fl) != 0) {
    const std::string why = std::strerror(errno);
    ::close(fd);
    ::unlink(tmp.c_str());
    return "cannot lock " + tmp + ": " + why;
  }
  sweep_dead();
  if (::rename(tmp.c_str(), final.c_str()) != 0) {
    const std::string why = std::strerror(errno);
    ::close(fd);
    ::unlink(tmp.c_str());
    return "cannot publish " + final + ": " + why;
  }
  g_fd = fd;
  return {};
}

}  // namespace

std::uint64_t task_starttime(int pid, int tid)
{
  return starttime_of("/proc/" + std::to_string(pid) + "/task/" + std::to_string(tid) + "/stat");
}

std::string publish(const std::vector<Entry> & entries)
{
  if (g_fd < 0) {
    std::string why = create();
    if (!why.empty()) return why;
  }
  std::string text = std::string(kHeader) + "\n";
  for (const auto & e : entries) {
    text += e.node + "\t" + e.group + "\t" + e.name + "\t" + std::to_string(e.pid) + "\t" +
            std::to_string(e.tid) + "\t" + std::to_string(e.tid_start) + "\n";
  }
  if (
    ::pwrite(g_fd, text.data(), text.size(), 0) != static_cast<ssize_t>(text.size()) ||
    ::ftruncate(g_fd, static_cast<off_t>(text.size())) != 0) {
    return std::string("cannot write the record: ") + std::strerror(errno);
  }
  return {};
}

void forget_after_fork() noexcept
{
  if (g_fd >= 0) ::close(g_fd);
  g_fd = -1;
}

std::vector<Process> read_live()
{
  std::vector<Process> out;
  const std::string d = dir();
  DIR * dp = ::opendir(d.c_str());
  if (dp == nullptr) return out;
  while (const dirent * e = ::readdir(dp)) {
    int pid = 0;
    if (!record_name(e->d_name, pid)) continue;
    const std::string path = d + "/" + e->d_name;
    if (!locked(path)) continue;
    const auto text = read_all(path);
    if (!text || text->rfind(kHeader, 0) != 0) continue;
    Process proc;
    proc.pid = pid;
    std::istringstream lines(*text);
    std::string line;
    std::getline(lines, line);
    while (std::getline(lines, line)) {
      std::istringstream cols(line);
      Entry en;
      std::string owner;
      std::string tid;
      std::string start;
      if (
        !std::getline(cols, en.node, '\t') || !std::getline(cols, en.group, '\t') ||
        !std::getline(cols, en.name, '\t') || !std::getline(cols, owner, '\t') ||
        !std::getline(cols, tid, '\t') || !std::getline(cols, start)) {
        continue;  // a torn line from a rewrite in progress
      }
      en.pid = std::atoi(owner.c_str());
      en.tid = std::atoi(tid.c_str());
      en.tid_start = std::strtoull(start.c_str(), nullptr, 10);
      if (
        en.pid <= 0 || en.tid <= 0 || en.tid_start == 0 ||
        task_starttime(en.pid, en.tid) != en.tid_start) {
        continue;  // the thread ended, or its tid now names another thread
      }
      proc.entries.push_back(std::move(en));
    }
    out.push_back(std::move(proc));
  }
  ::closedir(dp);
  return out;
}

}  // namespace cyra::record
