#include "host.hpp"

#include "parse_number.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string_view>

namespace capa::host
{

std::optional<std::string> Fs::read(const std::string & p) const
{
  const int fd = ::open(path(p).c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return std::nullopt;
  std::string out;
  char buf[4096];
  ssize_t n = 0;
  while ((n = ::read(fd, buf, sizeof buf)) > 0) out.append(buf, static_cast<std::size_t>(n));
  ::close(fd);
  if (n < 0) return std::nullopt;
  return out;
}

std::vector<std::string> Fs::list(const std::string & dir) const
{
  std::vector<std::string> out;
  DIR * d = ::opendir(path(dir).c_str());
  if (d == nullptr) return out;
  while (const dirent * e = ::readdir(d)) {
    const std::string name = e->d_name;
    if (name != "." && name != "..") out.push_back(name);
  }
  ::closedir(d);
  std::sort(out.begin(), out.end());
  return out;
}

bool Fs::exists(const std::string & p) const
{
  struct stat st
  {
  };
  return ::stat(path(p).c_str(), &st) == 0;
}

std::string trimmed(std::string s)
{
  while (!s.empty() && (s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
  std::size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
  return s.substr(i);
}

Cpus parse_cpu_list(const std::string & text)
{
  Cpus out;
  std::istringstream items(trimmed(text));
  std::string item;
  while (std::getline(items, item, ',')) {
    const std::string_view range(item);
    const auto dash = range.find('-');
    const auto first = detail::parse_number<std::uint32_t>(range.substr(0, dash));
    const auto last = dash == std::string_view::npos
                        ? first
                        : detail::parse_number<std::uint32_t>(range.substr(dash + 1));
    if (!first || !last) break;
    for (std::uint64_t c = *first; c <= *last; ++c) out.push_back(static_cast<std::uint32_t>(c));
  }
  return out;
}

// "ff,ffffffff": 32-bit hex words, most significant first.
Cpus parse_cpu_mask(const std::string & text)
{
  std::string hex;
  for (char ch : trimmed(text)) {
    if (ch != ',') hex += ch;
  }
  Cpus out;
  std::uint32_t bit = 0;
  for (auto it = hex.rbegin(); it != hex.rend(); ++it, bit += 4) {
    const int v = std::isxdigit(static_cast<unsigned char>(*it)) != 0
                    ? std::stoi(std::string(1, *it), nullptr, 16)
                    : 0;
    for (std::uint32_t b = 0; b < 4; ++b) {
      if ((v >> b) & 1) out.push_back(bit + b);
    }
  }
  return out;
}

bool contains(const Cpus & set, std::uint32_t cpu)
{
  return std::find(set.begin(), set.end(), cpu) != set.end();
}

bool overlap(const Cpus & a, const Cpus & b)
{
  return std::any_of(a.begin(), a.end(), [&](auto c) { return contains(b, c); });
}

std::string join(const Cpus & cpus)
{
  std::string out;
  for (auto c : cpus) out += (out.empty() ? "" : ",") + std::to_string(c);
  return out;
}

std::optional<std::string> field(const std::string & text, const std::string & key)
{
  const std::string needle = key + ":";
  std::size_t pos = text.find(needle);
  while (pos != 0 && pos != std::string::npos && text[pos - 1] != '\n') {
    pos = text.find(needle, pos + 1);
  }
  if (pos == std::string::npos) return std::nullopt;
  const std::size_t begin = pos + needle.size();
  const std::size_t end = text.find('\n', begin);
  return trimmed(text.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
}

std::optional<std::string> boot_param(const std::string & cmdline, const std::string & key)
{
  const std::string needle = key + "=";
  std::size_t pos = cmdline.find(needle);
  while (pos != std::string::npos) {
    if (pos == 0 || cmdline[pos - 1] == ' ') {
      const std::size_t start = pos + needle.size();
      const std::size_t end = cmdline.find_first_of(" \n", start);
      return cmdline.substr(start, end == std::string::npos ? std::string::npos : end - start);
    }
    pos = cmdline.find(needle, pos + 1);
  }
  return std::nullopt;
}

std::vector<std::string> words(const std::string & text)
{
  std::vector<std::string> out;
  std::istringstream in(text);
  std::string w;
  while (in >> w) out.push_back(w);
  return out;
}

std::string list_some(const std::vector<std::string> & items, std::size_t most)
{
  std::string out;
  for (std::size_t i = 0; i < items.size() && i < most; ++i) {
    out += (out.empty() ? "" : ", ") + items[i];
  }
  if (items.size() > most) out += ", ... (" + std::to_string(items.size()) + " in all)";
  return out;
}

namespace
{
constexpr unsigned kPfKthread = 0x00200000;
}

std::optional<Task> task(const Fs & fs, int pid, int tid)
{
  const std::string dir = "/proc/" + std::to_string(pid) + "/task/" + std::to_string(tid);
  const auto stat = fs.read(dir + "/stat");
  const auto status = fs.read(dir + "/status");
  if (!stat || !status) return std::nullopt;
  const auto open = stat->find('(');
  const auto close = stat->rfind(')');
  if (open == std::string::npos || close == std::string::npos || close < open) return std::nullopt;
  const auto f = words(stat->substr(close + 1));
  if (f.size() < 39) return std::nullopt;
  Task t;
  t.pid = pid;
  t.tid = tid;
  t.comm = stat->substr(open + 1, close - open - 1);
  t.kernel = (std::stoul(f[6]) & kPfKthread) != 0;
  t.minflt = std::stoull(f[7]);
  t.majflt = std::stoull(f[9]);
  t.prio = std::stoi(f[15]);
  t.rt_priority = std::stoi(f[37]);
  t.policy = std::stoi(f[38]);
  t.cpus = parse_cpu_list(field(*status, "Cpus_allowed_list").value_or(""));
  return t;
}

std::vector<Task> tasks(const Fs & fs, bool every_thread)
{
  std::vector<Task> out;
  for (const auto & p : fs.list("/proc")) {
    const auto pid = detail::parse_number<int>(p);
    if (!pid) continue;
    std::vector<int> tids{*pid};
    if (every_thread) {
      tids.clear();
      for (const auto & t : fs.list("/proc/" + p + "/task")) {
        if (const auto tid = detail::parse_number<int>(t)) tids.push_back(*tid);
      }
    }
    for (int tid : tids) {
      if (auto t = task(fs, *pid, tid)) out.push_back(std::move(*t));
    }
  }
  return out;
}

std::string cgroup_dir(const Fs & fs, int pid)
{
  const auto text = fs.read("/proc/" + (pid == 0 ? "self" : std::to_string(pid)) + "/cgroup");
  if (!text) return {};
  std::istringstream lines(*text);
  std::string line;
  while (std::getline(lines, line)) {
    if (line.rfind("0::", 0) == 0) {
      const std::string rel = line.substr(3);
      return "/sys/fs/cgroup" + (rel == "/" ? "" : rel);
    }
  }
  return {};
}

std::vector<std::string> cgroup_chain(const std::string & dir)
{
  std::vector<std::string> out;
  std::string d = dir;
  while (d.size() >= std::string("/sys/fs/cgroup").size()) {
    out.push_back(d);
    if (d == "/sys/fs/cgroup") break;
    d = d.substr(0, d.rfind('/'));
  }
  return out;
}

Cpus isolated_cpus(const Fs & fs)
{
  Cpus out = parse_cpu_list(fs.read("/sys/devices/system/cpu/isolated").value_or(""));
  for (auto c : parse_cpu_list(fs.read("/sys/fs/cgroup/cpuset.cpus.isolated").value_or(""))) {
    if (!contains(out, c)) out.push_back(c);
  }
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace capa::host
