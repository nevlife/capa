#include "cyra/spec.hpp"

#include "host.hpp"
#include "parse_number.hpp"
#include "record.hpp"

#include <dirent.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace cyra
{

namespace detail
{

const std::vector<std::vector<std::size_t>> & chains(const Spec & spec)
{
  return spec.chains_;
}

std::string stage_name(const Stage & stage)
{
  if (!stage.name.empty()) return stage.name;
  return stage.node + (stage.group.empty() ? "" : "/" + stage.group);
}

void reject_external(const Stage & stage, const char * caller)
{
  if (!stage.external) return;
  throw std::invalid_argument(
    "cyra: stage " + stage_name(stage) +
    " is declared external, so cyra does not set it; do not hand it to " + caller);
}

}  // namespace detail

namespace
{

[[noreturn]] void fail(const std::string & why)
{
  throw std::runtime_error("cyra: spec: " + why);
}

// Every key a node may carry is listed, and anything else is refused. That is why the file has no
// version field: a version number announces a schema change but says nothing about `priorty`.
void reject_unknown_keys(
  const YAML::Node & map, const std::set<std::string> & known, const std::string & where)
{
  for (const auto & kv : map) {
    const auto key = kv.first.as<std::string>();
    if (known.count(key) == 0) fail("unknown key '" + key + "' in " + where);
  }
}

Policy parse_policy(const std::string & s, const std::string & where)
{
  if (s == "inherit") return Policy::Inherit;
  if (s == "other") return Policy::Other;
  if (s == "fifo") return Policy::Fifo;
  if (s == "rr") return Policy::RoundRobin;
  fail("unknown policy '" + s + "' in " + where + " (inherit / other / fifo / rr)");
}

Kind parse_kind(const std::string & s, const std::string & where)
{
  if (s == "worker") return Kind::Worker;
  if (s == "release") return Kind::Release;
  fail("unknown kind '" + s + "' in " + where + " (worker / release)");
}

Children parse_children(const std::string & s, const std::string & where)
{
  if (s == "reset") return Children::Reset;
  if (s == "inherit") return Children::Inherit;
  fail("unknown children '" + s + "' in " + where + " (reset / inherit)");
}

Strictness parse_target(const YAML::Node & chain, const std::string & where)
{
  if (!chain["target"]) fail("chain " + where + " has no target (hard / soft)");
  const auto t = chain["target"].as<std::string>();
  if (t == "hard") return Strictness::Hard;
  if (t == "soft") return Strictness::Soft;
  fail("unknown target '" + t + "' in chain " + where + " (hard / soft)");
}

// A SCHED_OTHER thread takes no part in RT priority, so it sits outside every ordering rule.
// Requiring it to keep climbing would reject a soft consumer at the end of a chain.
bool is_rt(const Stage & s)
{
  if (s.external) return s.expect_priority > 0;
  return s.opts.policy == Policy::Fifo || s.opts.policy == Policy::RoundRobin;
}

int priority(const Stage & s)
{
  return s.external ? s.expect_priority : s.opts.priority;
}

// No cpus means any cpu, and an external stage's cpus are not in the file: both may land on any
// core, so both are treated as sharing every one.
bool share_a_cpu(const Stage & a, const Stage & b)
{
  if (a.external || b.external || a.opts.cpus.empty() || b.opts.cpus.empty()) return true;
  for (auto c : a.opts.cpus) {
    if (std::find(b.opts.cpus.begin(), b.opts.cpus.end(), c) != b.opts.cpus.end()) return true;
  }
  return false;
}

Stage parse_stage(const YAML::Node & n, const std::string & chain)
{
  if (!n.IsMap()) fail("a stage in chain " + chain + " is not a mapping");
  reject_unknown_keys(
    n,
    {"node", "group", "name", "kind", "external", "expect_priority", "policy", "priority", "cpus",
     "children", "match", "children_cpus"},
    "a stage of chain " + chain);

  Stage st;
  st.chain = chain;
  if (n["node"] && n["name"]) fail("a stage in chain " + chain + " has both node and name");
  if (n["name"]) {
    st.name = n["name"].as<std::string>();
    if (st.name.empty() || st.name.front() == '/') {
      fail(
        "stage name '" + st.name + "' in chain " + chain +
        " must be non-empty and must not start with '/', which is how node stages are spelled");
    }
    if (n["group"]) fail("stage " + st.name + " in chain " + chain + " is a name, not a node");
  } else if (n["node"]) {
    st.node = n["node"].as<std::string>();
    if (st.node.empty() || st.node.front() != '/') {
      fail("stage node '" + st.node + "' in chain " + chain + " must be a fully-qualified name");
    }
    if (n["group"]) st.group = n["group"].as<std::string>();
  } else {
    fail("a stage in chain " + chain + " has neither node nor name");
  }
  const std::string where = detail::stage_name(st);

  if (n["kind"]) st.kind = parse_kind(n["kind"].as<std::string>(), where);

  st.external = n["external"] && n["external"].as<bool>();
  if (st.external) {
    if (n["policy"] || n["priority"] || n["cpus"] || n["children"] || n["children_cpus"]) {
      fail(
        "external stage " + where +
        " must not carry policy/priority/cpus/children; cyra does not set it. Use "
        "expect_priority to record what it should already be running at");
    }
    if (n["match"]) {
      fail(
        "external stage " + where +
        " must not carry match: a matched stage is one the daemon sets, and external says "
        "nobody does");
    }
    if (n["expect_priority"]) st.expect_priority = n["expect_priority"].as<int>();
    if (st.expect_priority < 0 || st.expect_priority > 99) {
      fail(
        "external stage " + where + ": expect_priority " + std::to_string(st.expect_priority) +
        " is outside 1..99 (0 = not declared)");
    }
    return st;
  }

  if (n["expect_priority"]) {
    fail(
      "stage " + where +
      " is not external, so expect_priority does not apply; cyra sets this thread and reads "
      "back what it got");
  }
  if (n["policy"]) st.opts.policy = parse_policy(n["policy"].as<std::string>(), where);
  if (n["priority"]) st.opts.priority = n["priority"].as<int>();
  if (n["cpus"]) {
    for (const auto & c : n["cpus"]) st.opts.cpus.push_back(c.as<std::uint32_t>());
  }
  if (n["children"]) st.opts.children = parse_children(n["children"].as<std::string>(), where);
  try {
    st.opts.validate();
  } catch (const std::exception & e) {
    fail("stage " + where + ": " + e.what());
  }
  if (n["match"]) {
    const YAML::Node & m = n["match"];
    if (!m.IsMap()) fail("stage " + where + ": match is not a mapping");
    reject_unknown_keys(m, {"irq", "process", "thread"}, "the match of stage " + where);
    if (m["irq"]) st.match.irq = m["irq"].as<std::string>();
    if (m["process"]) st.match.process = m["process"].as<std::string>();
    if (m["thread"]) st.match.thread = m["thread"].as<std::string>();
    const bool by_irq = !st.match.irq.empty();
    const bool by_comm = !st.match.process.empty() && !st.match.thread.empty();
    if (by_irq == by_comm || (by_irq && (m["process"] || m["thread"]))) {
      fail(
        "stage " + where +
        ": match is either {irq: <name>} or {process: <comm>, thread: "
        "<comm>}");
    }
    if (st.opts.policy == Policy::Inherit && st.opts.cpus.empty()) {
      fail(
        "stage " + where +
        ": a matched stage with no policy and no cpus gives the daemon nothing to set");
    }
  }
  if (n["children_cpus"]) {
    for (const auto & c : n["children_cpus"]) st.children_cpus.push_back(c.as<std::uint32_t>());
    if (st.opts.cpus.empty() || st.opts.children != Children::Reset) {
      fail(
        "stage " + where +
        ": children_cpus moves reset children off the stage's cpus, so it needs cpus and "
        "children: reset");
    }
    for (auto c : st.children_cpus) {
      if (c >= 1024)
        fail("stage " + where + ": children_cpus " + std::to_string(c) + " is past the cpu set");
      if (std::find(st.opts.cpus.begin(), st.opts.cpus.end(), c) != st.opts.cpus.end()) {
        fail(
          "stage " + where + ": children_cpus includes cpu " + std::to_string(c) +
          ", one of the stage's own; the children would stay where they are");
      }
    }
  }
  return st;
}

bool same_declaration(const Stage & a, const Stage & b)
{
  return a.kind == b.kind && a.external == b.external && a.expect_priority == b.expect_priority &&
         a.opts.policy == b.opts.policy && a.opts.priority == b.opts.priority &&
         a.opts.cpus == b.opts.cpus && a.opts.children == b.opts.children &&
         a.match.irq == b.match.irq && a.match.process == b.match.process &&
         a.match.thread == b.match.thread && a.children_cpus == b.children_cpus;
}

struct Chain
{
  std::string name;
  std::vector<std::size_t> members;  // indices into the spec's stages, in flow order
};

std::string at(const Stage & s)
{
  return detail::stage_name(s) + " (" + std::to_string(priority(s)) + ")";
}

// Upstream must not preempt downstream where the two can meet on a cpu: a new instance's early
// stage would overtake the previous instance's late one. Equal is fine, since FIFO does not
// preempt at equal priority. Stages on disjoint cpus never preempt each other, so no rule binds
// them.
void check_flow(const std::vector<Stage> & stages, const Chain & chain)
{
  for (std::size_t i = 0; i < chain.members.size(); ++i) {
    const Stage & up = stages[chain.members[i]];
    if (up.kind != Kind::Worker || !is_rt(up)) continue;
    for (std::size_t j = i + 1; j < chain.members.size(); ++j) {
      const Stage & down = stages[chain.members[j]];
      if (down.kind != Kind::Worker || !is_rt(down) || !share_a_cpu(up, down)) continue;
      if (priority(down) < priority(up)) {
        fail(
          "chain " + chain.name + ": " + at(up) + " runs above " + at(down) +
          ", which is downstream of it on a cpu they share. An upstream stage that preempts a "
          "downstream one starves it");
      }
    }
  }
}

// A release thread only hands work on. Below the worker it feeds on the same cpu, the work is
// released late or a timer job is skipped.
void check_release(const std::vector<Stage> & stages, const Chain & chain)
{
  for (auto ri : chain.members) {
    const Stage & r = stages[ri];
    if (r.kind != Kind::Release || !is_rt(r)) continue;
    for (auto wi : chain.members) {
      const Stage & w = stages[wi];
      if (w.kind != Kind::Worker || !is_rt(w) || !share_a_cpu(r, w)) continue;
      if (priority(r) <= priority(w)) {
        fail(
          "chain " + chain.name + ": release stage " + at(r) + " does not run above worker " +
          at(w) +
          " on a cpu they share. A release thread below the work it releases hands it "
          "on late");
      }
    }
  }
}

// On a shared cpu the more critical chain comes first, whole. A stage's criticality is the
// highest among the chains it sits on.
void check_criticality(const std::vector<Stage> & stages, const std::vector<int> & crit)
{
  for (std::size_t a = 0; a < stages.size(); ++a) {
    for (std::size_t b = 0; b < stages.size(); ++b) {
      if (a == b || crit[a] <= crit[b]) continue;
      if (!is_rt(stages[a]) || !is_rt(stages[b]) || !share_a_cpu(stages[a], stages[b])) continue;
      if (priority(stages[a]) <= priority(stages[b])) {
        fail(
          at(stages[a]) + " sits on a chain of criticality " + std::to_string(crit[a]) +
          " and does not run above " + at(stages[b]) + " (criticality " + std::to_string(crit[b]) +
          ") on a cpu they share. The more critical chain comes first on a shared cpu");
      }
    }
  }
}

bool rt_worker(const Stage & s)
{
  return !s.external && s.kind == Kind::Worker && is_rt(s);
}

bool pinned_together(const Stage & a, const Stage & b)
{
  for (auto c : a.opts.cpus) {
    if (std::find(b.opts.cpus.begin(), b.opts.cpus.end(), c) != b.opts.cpus.end()) return true;
  }
  return false;
}

// SCHED_FIFO has no timeslice, so of two RT workers pinned to one core the first there holds it
// until it blocks; splitting them into groups was meant to stop exactly that. It also breaks
// nohz_full, whose condition is at most one runnable task. Refused only when a hard chain is
// involved: under soft RT two RT threads on one core is ordinary Linux. Release threads are short
// and block at once, so they do not count.
void check_cpu_exclusivity(const std::vector<Stage> & stages)
{
  for (std::size_t i = 0; i < stages.size(); ++i) {
    for (std::size_t j = i + 1; j < stages.size(); ++j) {
      const Stage & a = stages[i];
      const Stage & b = stages[j];
      if (!rt_worker(a) || !rt_worker(b) || !pinned_together(a, b)) continue;
      if (a.strict != Strictness::Hard && b.strict != Strictness::Hard) continue;
      fail(
        "stages " + detail::stage_name(a) + " and " + detail::stage_name(b) +
        " are both RT workers pinned to the same cpu. SCHED_FIFO has no timeslice, so one holds "
        "the core until it blocks; a hard chain gets one RT worker per core. Give them separate "
        "cpus, or declare the chain soft");
    }
  }
}

}  // namespace

Spec Spec::load(const std::string & path)
{
  YAML::Node root;
  try {
    root = YAML::LoadFile(path);
  } catch (const std::exception & e) {
    fail("cannot read '" + path + "': " + e.what());
  }
  if (!root.IsMap()) fail("'" + path + "' is not a mapping");
  reject_unknown_keys(root, {"chains"}, "the file root");
  if (!root["chains"] || !root["chains"].IsMap()) fail("'" + path + "' has no chains mapping");

  Spec spec;
  std::vector<Chain> chains;
  std::vector<int> crit;
  try {
    for (const auto & kv : root["chains"]) {
      Chain chain{kv.first.as<std::string>(), {}};
      const YAML::Node & body = kv.second;
      if (!body.IsMap()) fail("chain " + chain.name + " is not a mapping");
      reject_unknown_keys(body, {"target", "stages", "criticality"}, "chain " + chain.name);
      const Strictness strict = parse_target(body, chain.name);
      const int criticality = body["criticality"] ? body["criticality"].as<int>() : 0;
      if (criticality < 0) fail("chain " + chain.name + ": criticality must be 0 or more");
      if (!body["stages"] || !body["stages"].IsSequence() || body["stages"].size() == 0) {
        fail("chain " + chain.name + " has no stages sequence");
      }

      for (const auto & n : body["stages"]) {
        Stage st = parse_stage(n, chain.name);
        st.strict = strict;
        auto dup = std::find_if(spec.stages_.begin(), spec.stages_.end(), [&](const Stage & s) {
          return s.node == st.node && s.group == st.group && s.name == st.name;
        });
        if (dup == spec.stages_.end()) {
          chain.members.push_back(spec.stages_.size());
          crit.push_back(criticality);
          spec.stages_.push_back(std::move(st));
          continue;
        }
        // One stage on two chains is normal (a camera feeding control and logging). Two chains
        // asking it to run differently is the conflict, and only a file holding both sees it.
        if (!same_declaration(*dup, st)) {
          fail(
            "stage " + detail::stage_name(st) + " is declared differently by chains " + dup->chain +
            " and " + st.chain);
        }
        const auto index = static_cast<std::size_t>(dup - spec.stages_.begin());
        if (strict == Strictness::Hard) dup->strict = Strictness::Hard;
        crit[index] = std::max(crit[index], criticality);
        chain.members.push_back(index);
      }
      chains.push_back(std::move(chain));
    }
  } catch (const YAML::Exception & e) {
    fail("'" + path + "': " + e.what());
  }

  // Messages, the claims of this process, the records and the daemon all name a stage by how it
  // is spelled, so two stages spelled alike would be taken for one.
  std::map<std::string, const Stage *> spelled;
  for (const Stage & st : spec.stages_) {
    const auto [it, fresh] = spelled.emplace(detail::stage_name(st), &st);
    if (fresh) continue;
    auto which = [](const Stage & x) {
      return "node " + x.node + (x.group.empty() ? " (default group)" : " group " + x.group);
    };
    fail(
      "two stages are both named " + it->first + ": " + which(*it->second) + " and " + which(st) +
      "; rename one so each stage has its own name");
  }

  for (const auto & chain : chains) {
    check_flow(spec.stages_, chain);
    check_release(spec.stages_, chain);
  }
  check_criticality(spec.stages_, crit);
  check_cpu_exclusivity(spec.stages_);
  for (auto & chain : chains) spec.chains_.push_back(std::move(chain.members));
  return spec;
}

Spec Spec::load()
{
  const char * path = std::getenv("CYRA_SPEC");
  if (path == nullptr || *path == '\0') return Spec{};
  return load(path);
}

const Stage * Spec::find(const std::string & target, const std::string & group) const noexcept
{
  const bool node = !target.empty() && target.front() == '/';
  for (const auto & st : stages_) {
    if (node ? (st.node == target && st.group == group) : (st.name == target && group.empty())) {
      return &st;
    }
  }
  return nullptr;
}

const Stage & Spec::stage(const std::string & target, const std::string & group) const
{
  const Stage * st = find(target, group);
  if (st == nullptr) {
    fail(
      "no stage '" + target + (group.empty() ? "" : "/" + group) +
      "' in the spec; a label the file never names is a typo, and applying nothing would hide it");
  }
  return *st;
}

std::vector<const Stage *> Spec::external() const
{
  std::vector<const Stage *> out;
  for (const auto & st : stages_) {
    if (st.external) out.push_back(&st);
  }
  return out;
}

namespace
{

// Which live thread holds each stage. Leaked so a thread still exiting after main has returned
// does not release into a destroyed map.
struct Hold
{
  int tid = 0;
  std::string node;
  std::string group;
  std::string name;
  std::vector<record::Task> earlier;  // the process's threads before the bind: not its children
};
struct Claims
{
  std::mutex m;
  std::map<std::string, Hold> by_stage;
};
Claims & claims()
{
  static Claims * c = new Claims;
  return *c;
}

// Called with claims().m held. The record is what other processes read; this process's own
// checks use the map.
std::string republish_locked()
{
  std::vector<record::Entry> entries;
  for (const auto & [stage, h] : claims().by_stage) {
    entries.push_back(
      {h.node, h.group, h.name, ::getpid(), h.tid, record::task_starttime(::getpid(), h.tid),
       h.earlier});
  }
  return record::publish(record::Writer::Bind, entries);
}

// The stage this thread holds, released when the thread exits so a respawned thread can take it.
struct Held
{
  std::string name;
  Held() = default;
  Held(const Held &) = delete;
  Held & operator=(const Held &) = delete;
  ~Held()
  {
    if (name.empty()) return;
    std::lock_guard<std::mutex> lock(claims().m);
    claims().by_stage.erase(name);
    republish_locked();
  }
};
thread_local Held held;

// A forked child has one thread and none of the parent's stages, and the parent's record is not
// its to write. Holding the lock across fork keeps the child from inheriting a locked mutex.
void on_fork_prepare()
{
  claims().m.lock();
}
void on_fork_parent()
{
  claims().m.unlock();
}
void on_fork_child()
{
  claims().by_stage.clear();
  held.name.clear();
  record::forget_after_fork();
  claims().m.unlock();
}

}  // namespace

// One stage names one thread. Two threads under one declaration would leave the file describing
// neither, so the second is refused while the first is alive.
Report apply_checked(const Stage & stage)
{
  detail::reject_external(stage, "apply_checked()");
  if (!stage.match.empty()) {
    throw std::invalid_argument(
      "cyra: stage " + detail::stage_name(stage) +
      " carries match, so the daemon sets it; binding it here would give it two owners");
  }
  static std::once_flag atfork;
  std::call_once(atfork, [] { ::pthread_atfork(on_fork_prepare, on_fork_parent, on_fork_child); });

  const std::string name = detail::stage_name(stage);
  // Taken before the thread is set, so a thread it starts afterwards is never among them.
  std::vector<record::Task> earlier = record::tasks_of(::getpid());
  const int self = this_tid();
  earlier.erase(
    std::remove_if(
      earlier.begin(), earlier.end(), [&](const record::Task & t) { return t.first == self; }),
    earlier.end());
  Claims & c = claims();
  {
    std::lock_guard<std::mutex> lock(c.m);
    auto it = c.by_stage.find(name);
    if (it != c.by_stage.end() && name != held.name) {
      throw std::invalid_argument(
        "cyra: stage " + name + " is already applied on thread " + std::to_string(it->second.tid) +
        "; one stage declares one thread");
    }
    // The same thread again: what it started since the first bind is still its children.
    if (it != c.by_stage.end()) earlier = std::move(it->second.earlier);
    if (!held.name.empty()) c.by_stage.erase(held.name);
    c.by_stage[name] = Hold{self, stage.node, stage.group, stage.name, std::move(earlier)};
    held.name = name;
  }
  Report rep;
  try {
    rep = apply_checked(stage.opts, stage.strict);
  } catch (...) {
    std::lock_guard<std::mutex> lock(c.m);
    c.by_stage.erase(name);
    held.name.clear();
    throw;
  }
  for (auto & f : rep.findings) {
    if (f.id == "priority-order") {
      f = {"priority-order", Verdict::Ok, "checked against the whole chain file at load"};
    }
  }
  std::lock_guard<std::mutex> lock(c.m);
  const std::string why = republish_locked();
  if (!why.empty()) {
    rep.findings.push_back(
      {"record", Verdict::Unknown,
       "applied but not recorded (" + why +
         "): other processes, the CLI included, cannot see "
         "this thread hold " +
         name});
  }
  return rep;
}

std::vector<Binding> bindings()
{
  std::vector<Binding> out;
  for (const auto & proc : record::read_live()) {
    for (const auto & e : proc.entries) out.push_back({e.node, e.group, e.name, e.pid, e.tid});
  }
  return out;
}

std::uint64_t detail::task_starttime(int pid, int tid)
{
  return record::task_starttime(pid, tid);
}

std::string detail::record_bindings(const std::vector<Binding> & held)
{
  std::vector<record::Entry> entries;
  entries.reserve(held.size());
  for (const auto & b : held) {
    entries.push_back(
      {b.node, b.group, b.name, b.pid, b.tid, record::task_starttime(b.pid, b.tid), {}});
  }
  return record::publish(record::Writer::Daemon, entries);
}

std::string detail::daemon_dir()
{
  std::string why;
  std::string d = record::daemon_dir(why);
  if (!why.empty()) throw std::runtime_error("cyra: " + why);
  return d;
}

std::vector<detail::HeldStage> detail::held_stages()
{
  std::vector<HeldStage> out;
  for (const auto & proc : record::read_live()) {
    for (const auto & e : proc.entries) {
      out.push_back({{e.node, e.group, e.name, e.pid, e.tid}, e.earlier});
    }
  }
  return out;
}

Report bind(const std::string & target, const std::string & group)
{
  const Spec spec = Spec::load();
  if (spec.empty()) return Report{};
  return apply_checked(spec.stage(target, group));
}

namespace
{

const char * sched_name(int policy)
{
  switch (policy) {
    case SCHED_OTHER:
      return "SCHED_OTHER";
    case SCHED_FIFO:
      return "SCHED_FIFO";
    case SCHED_RR:
      return "SCHED_RR";
    case SCHED_BATCH:
      return "SCHED_BATCH";
    case SCHED_IDLE:
      return "SCHED_IDLE";
    default:
      return "an unnamed policy";
  }
}

std::string cpu_list(const std::vector<std::uint32_t> & cpus)
{
  if (cpus.empty()) return "every cpu";
  std::string out;
  for (auto c : cpus) out += (out.empty() ? "" : ",") + std::to_string(c);
  return out;
}

void add(Report & rep, const char * id, Verdict v, std::string detail)
{
  rep.findings.push_back(Finding{id, v, std::move(detail)});
}

int sched_policy(Policy p)
{
  return p == Policy::Fifo ? SCHED_FIFO : p == Policy::RoundRobin ? SCHED_RR : SCHED_OTHER;
}

bool within(
  const std::vector<std::uint32_t> & observed, const std::vector<std::uint32_t> & declared)
{
  return std::all_of(observed.begin(), observed.end(), [&](auto c) {
    return std::find(declared.begin(), declared.end(), c) != declared.end();
  });
}

bool runs_as(const Stage & stage, const ThreadState & st)
{
  if (stage.external) return stage.expect_priority > 0 && stage.expect_priority == st.priority;
  if (sched_policy(stage.opts.policy) != st.policy || stage.opts.priority != st.priority) {
    return false;
  }
  return stage.opts.cpus.empty() || within(st.cpus, stage.opts.cpus);
}

// The thread holding a stage is explained by it, whatever it runs at: a holder that drifted is
// verify()'s finding, not an unexplained thread. A child of a stage whose children inherit is
// explained while it runs at that stage's scheduling. Any other thread at a stage's values got
// there without cyra.
bool explains(
  const Stage & stage, const ThreadState & st, int tid, const std::map<std::string, int> & held_by)
{
  auto it = held_by.find(detail::stage_name(stage));
  if (it != held_by.end() && it->second == tid) return true;
  if (!runs_as(stage, st)) return false;
  return stage.external || stage.opts.children == Children::Inherit;
}

}  // namespace

Report verify(const Stage & stage, int tid, std::chrono::milliseconds window)
{
  const std::string where = detail::stage_name(stage);
  Report rep;

  ThreadState st;
  try {
    st = observe(tid);
  } catch (const std::exception & e) {
    // Not an omitted finding: a stage whose thread cannot be read is the case where a declaration
    // silently covers nothing, which is what declaring it was for.
    add(rep, "observed-thread", Verdict::Fail, "stage " + where + " cannot be read: " + e.what());
    return rep;
  }

  // An external stage says only what priority it should already be at, so a claim about policy
  // has to be derived from that: a nonzero RT priority is only reachable under an RT policy.
  const bool rt_expected = is_rt(stage);
  const int want_priority = priority(stage);
  const bool observed_rt = st.policy == SCHED_FIFO || st.policy == SCHED_RR;

  if (stage.external) {
    if (!rt_expected) {
      add(
        rep, "observed-policy", Verdict::Unknown,
        "external stage " + where +
          " declares no expect_priority, so the file claims nothing about its policy; it runs " +
          sched_name(st.policy));
    } else if (!observed_rt) {
      add(
        rep, "observed-policy", Verdict::Fail,
        "external stage " + where + " is expected at RT priority " + std::to_string(want_priority) +
          ", but its thread runs " + sched_name(st.policy));
    } else {
      add(rep, "observed-policy", Verdict::Ok, where + " runs " + sched_name(st.policy));
    }
  } else if (stage.opts.policy == Policy::Inherit) {
    add(
      rep, "observed-policy", Verdict::Unknown,
      "stage " + where + " asks for no policy, so there is nothing to check; it runs " +
        sched_name(st.policy));
  } else {
    const int want = sched_policy(stage.opts.policy);
    add(
      rep, "observed-policy", want == st.policy ? Verdict::Ok : Verdict::Fail,
      "stage " + where + " declares " + sched_name(want) + " and its thread runs " +
        sched_name(st.policy));
  }

  if (!rt_expected) {
    add(
      rep, "observed-priority", Verdict::Unknown,
      "stage " + where + " declares no RT priority; its thread reports " +
        std::to_string(st.priority));
  } else {
    add(
      rep, "observed-priority", want_priority == st.priority ? Verdict::Ok : Verdict::Fail,
      "stage " + where + " declares priority " + std::to_string(want_priority) +
        " and its thread runs at " + std::to_string(st.priority));
  }

  if (stage.external || !rt_expected) {
    add(
      rep, "observed-children", Verdict::Unknown,
      "stage " + where + " passes no RT policy on, so children are not checked");
  } else {
    const bool want_reset = stage.opts.children == Children::Reset;
    add(
      rep, "observed-children", want_reset == st.reset_on_fork ? Verdict::Ok : Verdict::Fail,
      "stage " + where + " declares children=" + (want_reset ? "reset" : "inherit") +
        " and its thread has SCHED_RESET_ON_FORK " + (st.reset_on_fork ? "set" : "clear"));
  }

  // Affinity is checked only where cyra set it. The claim is observed within declared, the reading
  // apply_checked confirms: a cpuset may narrow the mask, and a thread on fewer of its declared
  // cpus has left none of them.
  if (stage.external || stage.opts.cpus.empty()) {
    add(
      rep, "observed-cpus", Verdict::Unknown,
      "stage " + where + " pins no cpu; its thread may run on " + cpu_list(st.cpus));
  } else {
    add(
      rep, "observed-cpus", within(st.cpus, stage.opts.cpus) ? Verdict::Ok : Verdict::Fail,
      "stage " + where + " declares cpus " + cpu_list(stage.opts.cpus) +
        " and its thread may run on " + cpu_list(st.cpus));
  }
  host::verify_now(rep, tid, st);
  if (window.count() > 0) host::verify_window(rep, stage, tid, window);
  return rep;
}

namespace
{

// A reset child keeps the stage's affinity. On a hard stage's cpu, or a nohz_full one, it is a
// second runnable task there; children_cpus is how it leaves.
void shared_stage_cpus(
  Report & rep, const Spec & spec, const std::map<std::string, int> & held_by,
  const std::vector<std::pair<int, ThreadState>> & fair)
{
  const auto nohz =
    host::parse_cpu_list(host::Fs{}.read("/sys/devices/system/cpu/nohz_full").value_or(""));
  std::vector<std::string> crowded;
  for (const auto & s : spec.stages()) {
    if (held_by.count(detail::stage_name(s)) == 0 || s.opts.cpus.empty()) continue;
    if (!s.children_cpus.empty() || s.opts.children != Children::Reset) continue;
    if (s.strict != Strictness::Hard && !host::overlap(s.opts.cpus, nohz)) continue;
    for (const auto & [tid, st] : fair) {
      const bool holder = std::any_of(
        held_by.begin(), held_by.end(), [t = tid](const auto & h) { return h.second == t; });
      if (!holder && !st.cpus.empty() && within(st.cpus, s.opts.cpus)) {
        crowded.push_back("tid " + std::to_string(tid) + " on " + detail::stage_name(s));
      }
    }
  }
  if (crowded.empty()) {
    add(
      rep, "stage-cpu-shared", Verdict::Ok,
      "no reset child stays on a hard or nohz_full stage cpu");
  } else {
    add(
      rep, "stage-cpu-shared", Verdict::Warn,
      "SCHED_OTHER threads left on a hard or nohz_full stage's cpus: " +
        host::list_some(crowded, 3) +
        ". They compete for the cpu and keep the tick on; give the stage children_cpus");
  }
}

Report scan_threads(
  const Spec & spec, const std::string & task_dir, const std::map<std::string, int> & held_by)
{
  Report rep;
  DIR * d = ::opendir(task_dir.c_str());
  if (d == nullptr) {
    add(
      rep, "observed-process", Verdict::Fail,
      "cannot read " + task_dir + ": " + std::strerror(errno) +
        ". A process that is gone must not read as one with nothing undeclared");
    return rep;
  }
  int found = 0;
  std::vector<std::pair<int, ThreadState>> fair;
  std::vector<std::string> uring;
  while (const dirent * e = ::readdir(d)) {
    const auto parsed = detail::parse_number<int>(e->d_name);
    if (!parsed) continue;
    const int tid = *parsed;
    ThreadState st;
    try {
      st = observe(tid);
    } catch (const std::exception &) {
      continue;  // a thread that ended between the readdir and the read is not a finding
    }
    const std::string comm =
      host::trimmed(host::Fs{}.read(task_dir + "/" + e->d_name + "/comm").value_or(""));
    if (comm.rfind("iou-sqp-", 0) == 0 || comm.rfind("iou-wrk-", 0) == 0) {
      uring.push_back(comm + " (tid " + std::to_string(tid) + ", cpus " + cpu_list(st.cpus) + ")");
    }
    if (st.policy != SCHED_FIFO && st.policy != SCHED_RR) {
      fair.emplace_back(tid, st);
      continue;
    }
    const auto & stages = spec.stages();
    if (std::any_of(stages.begin(), stages.end(), [&](const Stage & s) {
          return explains(s, st, tid, held_by);
        })) {
      continue;
    }
    ++found;
    add(
      rep, "undeclared-rt-thread", Verdict::Warn,
      "tid " + std::to_string(tid) + " runs " + sched_name(st.policy) + " at priority " +
        std::to_string(st.priority) + " on cpus " + cpu_list(st.cpus) +
        ", which no stage of the file explains");
  }
  ::closedir(d);

  if (found == 0) {
    add(
      rep, "undeclared-rt-thread", Verdict::Ok,
      "every RT thread holds a stage or is a child a stage lets inherit");
  }
  if (uring.empty()) {
    add(rep, "io-uring", Verdict::Ok, "no io_uring helper thread");
  } else {
    add(
      rep, "io-uring", Verdict::Warn,
      "io_uring helper threads: " + host::list_some(uring, 3) +
        ". io-wq workers run SCHED_OTHER, and an SQPOLL thread without SQ_AFF inherits its "
        "creator's cpus, an isolated one included");
  }
  shared_stage_cpus(rep, spec, held_by, fair);
  return rep;
}

}  // namespace

Report verify_process(const Spec & spec)
{
  std::map<std::string, int> held_by;
  {
    std::lock_guard<std::mutex> lock(claims().m);
    for (const auto & [stage, h] : claims().by_stage) held_by[stage] = h.tid;
  }
  return scan_threads(spec, "/proc/self/task", held_by);
}

Report verify_process(const Spec & spec, int pid)
{
  if (pid <= 0) {
    throw std::invalid_argument("cyra: verify_process needs a pid; 0 would not name a process");
  }
  if (pid == ::getpid()) return verify_process(spec);

  std::map<std::string, int> held_by;
  bool recorded = false;
  for (const auto & proc : record::read_live()) {
    if (proc.pid == pid) recorded = true;
    for (const auto & e : proc.entries) {
      if (e.pid != pid) continue;
      recorded = true;  // the daemon's record, for a process that binds nothing itself
      Stage key;
      key.node = e.node;
      key.group = e.group;
      key.name = e.name;
      held_by[detail::stage_name(key)] = e.tid;
    }
  }
  Report rep = scan_threads(spec, "/proc/" + std::to_string(pid) + "/task", held_by);
  if (!recorded) {
    add(
      rep, "record", Verdict::Unknown,
      "process " + std::to_string(pid) +
        " has no live record naming its threads, so which of them hold a stage is not known");
  }
  return rep;
}

}  // namespace cyra
