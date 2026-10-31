#include "record.hpp"

#include "parse_number.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace capa::record
{

namespace
{

// Version 4 added `ask`. Version 3 records, from processes started before, still read.
constexpr const char * kMagic = "capa-record 4";
constexpr const char * kMagic3 = "capa-record 3";
constexpr std::size_t kMaxRecord = std::size_t{1} << 20;
constexpr int kReadTries = 3;

auto env_or(const char * name, const char * fallback) -> std::string
{
  const char * env = std::getenv(name);
  return env != nullptr && *env != '\0' ? env : fallback;
}

// /dev/shm: always there, shared by every user, nothing to install.
auto dir_of(Writer w) -> std::string
{
  return w == Writer::Bind ? env_or("CAPA_RUN_DIR", "/dev/shm")
                           : env_or("CAPA_DAEMON_DIR", "/run/capa");
}

auto read_fd(int fd) -> std::optional<std::string>
{
  std::string out;
  char buf[4096];
  off_t off = 0;
  ssize_t n = 0;
  while ((n = ::pread(fd, buf, sizeof buf, off)) > 0) {
    out.append(buf, static_cast<std::size_t>(n));
    off += n;
    if (out.size() > kMaxRecord) {
      return std::nullopt;
    }
  }
  if (n < 0) {
    return std::nullopt;
  }
  return out;
}

auto read_path(const std::string & path) -> std::optional<std::string>
{
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return std::nullopt;
  }
  auto text = read_fd(fd);
  ::close(fd);
  return text;
}

// Field 22 of a stat line. comm (field 2) may hold spaces and parens, so count from its last ')'.
auto starttime_of(const std::string & stat_path) -> std::uint64_t
{
  const auto text = read_path(stat_path);
  if (!text) {
    return 0;
  }
  const auto close = text->rfind(')');
  if (close == std::string::npos) {
    return 0;
  }
  std::istringstream rest(text->substr(close + 1));
  std::string field;
  for (int i = 3; i <= 22; ++i) {
    if (!(rest >> field)) {
      return 0;
    }
  }
  return std::strtoull(field.c_str(), nullptr, 10);
}

auto effective_uid(int pid) -> std::optional<uid_t>
{
  const auto status = read_path("/proc/" + std::to_string(pid) + "/status");
  if (!status) {
    return std::nullopt;
  }
  const auto at = status->find("\nUid:");
  if (at == std::string::npos) {
    return std::nullopt;
  }
  std::istringstream ids(status->substr(at + 5));
  unsigned long real = 0;
  unsigned long effective = 0;
  if (!(ids >> real >> effective)) {
    return std::nullopt;
  }
  return static_cast<uid_t>(effective);
}

// Catches a read in the middle of a rewrite. Who may write is settled by owner and directory.
auto fnv1a(const std::string & s) -> std::uint64_t
{
  std::uint64_t h = 14695981039346656037ULL;
  for (const unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ULL;
  }
  return h;
}

auto hex(std::uint64_t v) -> std::string
{
  char out[17];
  std::snprintf(out, sizeof out, "%016" PRIx64, v);
  return out;
}

auto frame(const std::string & body) -> std::string
{
  return std::string(kMagic) + " " + std::to_string(body.size()) + " " + hex(fnv1a(body)) + "\n" +
         body;
}

struct Framed
{
  std::string body;
  bool has_ask = true;
};

auto unframe(const std::string & text) -> std::optional<Framed>
{
  const std::size_t magic = std::strlen(kMagic);
  const auto nl = text.find('\n');
  if (nl == std::string::npos) {
    return std::nullopt;
  }
  const bool v4 = text.compare(0, magic, kMagic) == 0;
  if (!v4 && text.compare(0, std::strlen(kMagic3), kMagic3) != 0) {
    return std::nullopt;
  }
  std::istringstream head(text.substr(magic, nl - magic));
  std::size_t size = 0;
  std::string sum;
  if (!(head >> size >> sum)) {
    return std::nullopt;
  }
  std::string body = text.substr(nl + 1);
  if (body.size() != size || sum != hex(fnv1a(body))) {
    return std::nullopt;
  }
  return Framed{std::move(body), v4};
}

// A whole-file write lock held by any open file description is a live writer.
auto write_locked(int fd) -> bool
{
  struct flock fl
  {
  };
  fl.l_type = F_WRLCK;
  fl.l_whence = SEEK_SET;
  return ::fcntl(fd, F_OFD_GETLK, &fl) == 0 && fl.l_type != F_UNLCK;
}

// Only the final names count; a creator locks the file before renaming it into place, so a
// record under its final name is never seen unlocked while its writer lives.
auto record_name(std::string_view entry, int & pid, std::uint64_t & start) -> bool
{
  constexpr std::string_view prefix = "capa.";
  if (entry.substr(0, prefix.size()) != prefix) {
    return false;
  }
  const std::string_view rest = entry.substr(prefix.size());
  const auto dot = rest.find('.');
  if (dot == std::string_view::npos) {
    return false;
  }
  const auto p = detail::parse_number<int>(rest.substr(0, dot));
  const auto s = detail::parse_number<std::uint64_t>(rest.substr(dot + 1));
  if (!p || !s || *p <= 0) {
    return false;
  }
  pid = *p;
  start = *s;
  return true;
}

// A regular file, not reached through a link, writable by its owner only. O_NONBLOCK so that a
// fifo under a record's name cannot stall the open.
auto open_record(int dir, const char * name, struct stat & st) -> int
{
  const int fd = ::openat(dir, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    return -1;
  }
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || (st.st_mode & 022) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

auto earlier_field(const std::vector<Task> & tasks) -> std::string
{
  if (tasks.empty()) {
    return "-";
  }
  std::string out;
  for (const auto & [tid, start] : tasks) {
    if (!out.empty()) {
      out += ',';
    }
    out += std::to_string(tid) + ":" + std::to_string(start);
  }
  return out;
}

auto parse_earlier(const std::string & field) -> std::vector<Task>
{
  std::vector<Task> out;
  if (field == "-") {
    return out;
  }
  std::istringstream items(field);
  std::string item;
  while (std::getline(items, item, ',')) {
    const auto colon = item.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    const auto tid = detail::parse_number<int>(std::string_view(item).substr(0, colon));
    const auto start =
      detail::parse_number<std::uint64_t>(std::string_view(item).substr(colon + 1));
    if (tid && start) {
      out.emplace_back(*tid, *start);
    }
  }
  return out;
}

auto parse_entries(const std::string & body, bool has_ask) -> std::vector<Entry>
{
  std::vector<Entry> out;
  std::istringstream lines(body);
  std::string line;
  while (std::getline(lines, line)) {
    std::istringstream cols(line);
    Entry en;
    std::string owner;
    std::string tid;
    std::string start;
    std::string ask = "0";
    std::string earlier;
    if (
      !std::getline(cols, en.node, '\t') || !std::getline(cols, en.group, '\t') ||
      !std::getline(cols, en.name, '\t') || !std::getline(cols, owner, '\t') ||
      !std::getline(cols, tid, '\t') || !std::getline(cols, start, '\t') ||
      (has_ask && !std::getline(cols, ask, '\t')) || !std::getline(cols, earlier)) {
      continue;
    }
    const auto pid = detail::parse_number<int>(owner);
    const auto t = detail::parse_number<int>(tid);
    const auto s = detail::parse_number<std::uint64_t>(start);
    if (!pid || !t || !s) {
      continue;
    }
    en.pid = *pid;
    en.tid = *t;
    en.tid_start = *s;
    en.ask = ask == "1";
    en.earlier = parse_earlier(earlier);
    out.push_back(std::move(en));
  }
  return out;
}

void read_dir(Writer w, std::vector<Process> & out)
{
  const int flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | (w == Writer::Daemon ? O_NOFOLLOW : 0);
  const int dfd = ::open(dir_of(w).c_str(), flags);
  if (dfd < 0) {
    return;
  }
  struct stat dst
  {
  };
  if (::fstat(dfd, &dst) != 0 || (w == Writer::Daemon && (dst.st_mode & 022) != 0)) {
    ::close(dfd);
    return;
  }
  DIR * dp = ::fdopendir(dfd);
  if (dp == nullptr) {
    ::close(dfd);
    return;
  }
  while (const dirent * e = ::readdir(dp)) {
    int pid = 0;
    std::uint64_t start = 0;
    if (!record_name(e->d_name, pid, start)) {
      continue;
    }
    struct stat st
    {
    };
    const int fd = open_record(::dirfd(dp), e->d_name, st);
    if (fd < 0) {
      continue;
    }
    const bool trusted =
      write_locked(fd) && starttime_of("/proc/" + std::to_string(pid) + "/stat") == start &&
      (w == Writer::Daemon ? st.st_uid == dst.st_uid : effective_uid(pid) == st.st_uid);
    std::optional<Framed> body;
    for (int i = 0; trusted && !body && i < kReadTries; ++i) {
      const auto text = read_fd(fd);
      if (text) {
        body = unframe(*text);
      }
    }
    ::close(fd);
    if (!body) {
      continue;
    }

    Process proc;
    proc.pid = pid;
    for (auto & en : parse_entries(body->body, body->has_ask)) {
      if (w == Writer::Bind && en.pid != pid) {
        continue;
      }
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
}

// This user's records whose writer is gone. Other users' are theirs to sweep.
void sweep_dead(const std::string & d)
{
  const int dfd = ::open(d.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dfd < 0) {
    return;
  }
  DIR * dp = ::fdopendir(dfd);
  if (dp == nullptr) {
    ::close(dfd);
    return;
  }
  while (const dirent * e = ::readdir(dp)) {
    int pid = 0;
    std::uint64_t start = 0;
    if (!record_name(e->d_name, pid, start)) {
      continue;
    }
    struct stat st
    {
    };
    const int fd = open_record(::dirfd(dp), e->d_name, st);
    if (fd < 0) {
      continue;
    }
    const bool dead = st.st_uid == ::geteuid() && !write_locked(fd);
    ::close(fd);
    if (dead) {
      ::unlinkat(::dirfd(dp), e->d_name, 0);
    }
  }
  ::closedir(dp);
}

// Readers trust the daemon's directory, so it must be this user's and writable by no one else.
// Created if missing, as systemd's RuntimeDirectory= would.
auto check_daemon_dir(const std::string & d) -> std::string
{
  if (::mkdir(d.c_str(), 0755) != 0 && errno != EEXIST) {
    return "cannot create " + d + ": " + std::strerror(errno);
  }
  const int dfd = ::open(d.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (dfd < 0) {
    return "cannot open " + d + ": " + std::strerror(errno);
  }
  struct stat st
  {
  };
  const bool ok = ::fstat(dfd, &st) == 0 && st.st_uid == ::geteuid() && (st.st_mode & 022) == 0;
  ::close(dfd);
  if (!ok) {
    return d + " must belong to this user and be writable by no one else";
  }
  return {};
}

int g_fd[2] = {-1, -1};

auto create(Writer w) -> std::string
{
  const std::string d = dir_of(w);
  if (w == Writer::Daemon) {
    std::string why = check_daemon_dir(d);
    if (!why.empty()) {
      return why;
    }
  }
  std::string tmp = d + "/.capa.creating.XXXXXX";
  const int fd = ::mkostemp(tmp.data(), O_CLOEXEC);
  if (fd < 0) {
    return "cannot create a record in " + d + ": " + std::strerror(errno);
  }
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
  sweep_dead(d);
  const std::string final = d + "/capa." + std::to_string(::getpid()) + "." +
                            std::to_string(starttime_of("/proc/self/stat"));
  if (::rename(tmp.c_str(), final.c_str()) != 0) {
    const std::string why = std::strerror(errno);
    ::close(fd);
    ::unlink(tmp.c_str());
    return "cannot publish " + final + ": " + why;
  }
  g_fd[static_cast<int>(w)] = fd;
  return {};
}

}  // namespace

auto task_starttime(int pid, int tid) -> std::uint64_t
{
  return starttime_of("/proc/" + std::to_string(pid) + "/task/" + std::to_string(tid) + "/stat");
}

auto tasks_of(int pid) -> std::vector<Task>
{
  std::vector<Task> out;
  DIR * d = ::opendir(("/proc/" + std::to_string(pid) + "/task").c_str());
  if (d == nullptr) {
    return out;
  }
  while (const dirent * e = ::readdir(d)) {
    const auto tid = detail::parse_number<int>(e->d_name);
    if (!tid) {
      continue;
    }
    const std::uint64_t start = task_starttime(pid, *tid);
    if (start != 0) {
      out.emplace_back(*tid, start);
    }
  }
  ::closedir(d);
  return out;
}

auto daemon_dir(std::string & why) -> std::string
{
  const std::string d = dir_of(Writer::Daemon);
  why = check_daemon_dir(d);
  return why.empty() ? d : std::string();
}

auto publish(Writer writer, const std::vector<Entry> & entries) -> std::string
{
  int & fd = g_fd[static_cast<int>(writer)];
  if (fd < 0) {
    std::string why = create(writer);
    if (!why.empty()) {
      return why;
    }
  }
  std::string body;
  for (const auto & e : entries) {
    body += e.node + "\t" + e.group + "\t" + e.name + "\t" + std::to_string(e.pid) + "\t" +
            std::to_string(e.tid) + "\t" + std::to_string(e.tid_start) + "\t" +
            (e.ask ? "1" : "0") + "\t" + earlier_field(e.earlier) + "\n";
  }
  const std::string text = frame(body);
  if (
    ::pwrite(fd, text.data(), text.size(), 0) != static_cast<ssize_t>(text.size()) ||
    ::ftruncate(fd, static_cast<off_t>(text.size())) != 0) {
    return std::string("cannot write the record: ") + std::strerror(errno);
  }
  return {};
}

void forget_after_fork() noexcept
{
  for (int & fd : g_fd) {
    if (fd >= 0) {
      ::close(fd);
    }
    fd = -1;
  }
}

auto read_live() -> std::vector<Process>
{
  std::vector<Process> out;
  read_dir(Writer::Bind, out);
  read_dir(Writer::Daemon, out);
  return out;
}

}  // namespace capa::record
