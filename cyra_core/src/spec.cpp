#include "cyra/spec.hpp"

#include <dirent.h>
#include <sched.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cstdlib>
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

std::string stage_name(const std::string & node, const std::string & group)
{
  return node + (group.empty() ? "" : "/" + group);
}

void reject_external(const Stage & stage, const char * caller)
{
  if (!stage.external) return;
  throw std::invalid_argument(
    "cyra: stage " + stage_name(stage.node, stage.group) +
    " is declared external, so cyra does not set it; do not hand it to " + caller);
}

}  // namespace detail

namespace
{

[[noreturn]] void fail(const std::string & why)
{
  throw std::runtime_error("cyra: spec: " + why);
}

// Strict by construction: every key a node reads is removed from this set, and whatever is left
// was not understood. This is why the file carries no version field -- a version number announces
// a schema change but says nothing about `priorty`, and refusing what we did not read catches
// both.
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

Strictness parse_target(const YAML::Node & chain, const std::string & where)
{
  if (!chain["target"]) fail("chain " + where + " has no target (hard / soft)");
  const auto t = chain["target"].as<std::string>();
  if (t == "hard") return Strictness::Hard;
  if (t == "soft") return Strictness::Soft;
  fail("unknown target '" + t + "' in chain " + where + " (hard / soft)");
}

// What a stage runs at, whichever side owns it. Non-RT stages report 0 and sit outside the
// ordering rule entirely -- a SCHED_OTHER thread does not take part in RT priority at all, so
// requiring it to keep climbing would reject a soft consumer at the end of a chain.
int effective_priority(const Stage & s)
{
  return s.external ? s.expect_priority : s.opts.priority;
}

Stage parse_stage(const YAML::Node & n, const std::string & chain)
{
  if (!n.IsMap()) fail("a stage in chain " + chain + " is not a mapping");
  reject_unknown_keys(
    n, {"node", "group", "external", "expect_priority", "policy", "priority", "cpus"},
    "a stage of chain " + chain);

  Stage st;
  st.chain = chain;
  if (!n["node"]) fail("a stage in chain " + chain + " has no node");
  st.node = n["node"].as<std::string>();
  if (st.node.empty() || st.node.front() != '/') {
    fail("stage node '" + st.node + "' in chain " + chain + " must be a fully-qualified name");
  }
  if (n["group"]) st.group = n["group"].as<std::string>();
  const std::string where = detail::stage_name(st.node, st.group);

  st.external = n["external"] && n["external"].as<bool>();
  if (st.external) {
    // An external stage is a record, not a request. Carrying thread options here would read as
    // something cyra applies, and it never does.
    if (n["policy"] || n["priority"] || n["cpus"]) {
      fail(
        "external stage " + where +
        " must not carry policy/priority/cpus; cyra does not set "
        "it. Use expect_priority to record what it should already be running at");
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
      " is not external, so expect_priority does not apply; cyra sets this "
      "thread and reads back what it got");
  }
  if (n["policy"]) st.opts.policy = parse_policy(n["policy"].as<std::string>(), where);
  if (n["priority"]) st.opts.priority = n["priority"].as<int>();
  if (n["cpus"]) {
    for (const auto & c : n["cpus"]) st.opts.cpus.push_back(c.as<std::uint32_t>());
  }
  try {
    st.opts.validate();
  } catch (const std::exception & e) {
    fail("stage " + where + ": " + e.what());
  }
  return st;
}

// The ordering rule, and the reason the whole chain lives in one file. Upstream must never be
// able to preempt downstream, so RT priorities strictly increase along the flow. This is
// preflight's `priority-order` check (transport below control) generalized past two stages.
void check_order(const std::vector<Stage> & chain_stages, const std::string & chain)
{
  const Stage * prev = nullptr;
  for (const auto & st : chain_stages) {
    if (effective_priority(st) <= 0) continue;  // not on the RT ladder
    if (prev != nullptr && effective_priority(st) <= effective_priority(*prev)) {
      fail(
        "chain " + chain + " does not climb: " + prev->node + " runs at " +
        std::to_string(effective_priority(*prev)) + " and the stage after it, " + st.node +
        ", runs at " + std::to_string(effective_priority(st)) +
        ". An upstream stage that can preempt a downstream one starves it");
    }
    prev = &st;
  }
}

bool is_rt(const Stage & s)
{
  return !s.external && (s.opts.policy == Policy::Fifo || s.opts.policy == Policy::RoundRobin);
}

bool shares_a_cpu(const Stage & a, const Stage & b)
{
  for (auto c : a.opts.cpus) {
    if (std::find(b.opts.cpus.begin(), b.opts.cpus.end(), c) != b.opts.cpus.end()) return true;
  }
  return false;
}

// Two RT threads on one core is not isolation, it is the opposite. SCHED_FIFO has no timeslice,
// so the one that gets there first holds the core until it blocks -- the groups were split to
// stop exactly that, and pinning them together undoes the split. It also breaks nohz_full, whose
// condition is at most one runnable task on the cpu; two threads that wake together bring the
// tick back.
//
// Refused only when a hard chain is involved. Under soft RT this is a legitimate arrangement --
// two RT threads sharing a core with different priorities is ordinary Linux -- and the file
// cannot tell whether the core is isolated at all. Strictness is what separates the readings,
// the same split Strictness draws over preflight's Warn findings.
void check_cpu_exclusivity(const std::vector<Stage> & stages)
{
  for (std::size_t i = 0; i < stages.size(); ++i) {
    for (std::size_t j = i + 1; j < stages.size(); ++j) {
      const Stage & a = stages[i];
      const Stage & b = stages[j];
      if (!is_rt(a) || !is_rt(b) || !shares_a_cpu(a, b)) continue;
      if (a.strict != Strictness::Hard && b.strict != Strictness::Hard) continue;
      fail(
        "stages " + detail::stage_name(a.node, a.group) + " and " +
        detail::stage_name(b.node, b.group) +
        " are both pinned to the same cpu with an RT policy. SCHED_FIFO has no timeslice, so one "
        "holds the core until it blocks and the other cannot run; a hard chain gets one RT thread "
        "per core. Give them separate cpus, or declare the chain soft");
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
  if (!root["chains"]) fail("'" + path + "' has no chains");

  Spec spec;
  for (const auto & kv : root["chains"]) {
    const auto chain = kv.first.as<std::string>();
    const YAML::Node & body = kv.second;
    if (!body.IsMap()) fail("chain " + chain + " is not a mapping");
    reject_unknown_keys(body, {"target", "stages"}, "chain " + chain);
    const Strictness strict = parse_target(body, chain);
    if (!body["stages"] || !body["stages"].IsSequence()) {
      fail("chain " + chain + " has no stages sequence");
    }

    std::vector<Stage> chain_stages;
    for (const auto & n : body["stages"]) chain_stages.push_back(parse_stage(n, chain));
    if (chain_stages.empty()) fail("chain " + chain + " has no stages");
    check_order(chain_stages, chain);

    // The control loop is the chain's last RT stage by definition of the flow order, so every
    // earlier stage is checked against it and it is checked against nothing. Deriving it here is
    // what the file buys: a node declaring itself in isolation cannot know this number.
    int control = 0;
    for (const auto & st : chain_stages) {
      if (effective_priority(st) > 0) control = effective_priority(st);
    }
    for (auto & st : chain_stages) {
      st.strict = strict;
      st.control_priority = (effective_priority(st) == control) ? 0 : control;
    }

    for (auto & st : chain_stages) {
      auto dup = std::find_if(spec.stages_.begin(), spec.stages_.end(), [&](const Stage & s) {
        return s.node == st.node && s.group == st.group;
      });
      if (dup != spec.stages_.end()) {
        // A stage legitimately sits on more than one chain (a camera feeding both control and
        // logging). Two chains asking it to run differently is the conflict, and only a file
        // holding both can see it.
        if (
          dup->external != st.external || dup->expect_priority != st.expect_priority ||
          dup->opts.policy != st.opts.policy || dup->opts.priority != st.opts.priority ||
          dup->opts.cpus != st.opts.cpus) {
          fail(
            "stage " + detail::stage_name(st.node, st.group) +
            " is declared differently by chains " + dup->chain + " and " + st.chain);
        }
        // Same declaration on both chains: keep the first, but the reading of a Warn belongs to
        // the strictest chain the stage sits on. A camera feeding a hard control chain is a hard
        // stage no matter which chain the file lists first.
        if (st.strict == Strictness::Hard) dup->strict = Strictness::Hard;
        continue;
      }
      spec.stages_.push_back(std::move(st));
    }
  }
  // Across every chain, not per chain: two stages can land on one core from two declarations, and
  // that is precisely the collision a split file cannot see.
  check_cpu_exclusivity(spec.stages_);
  return spec;
}

Spec Spec::load()
{
  const char * path = std::getenv("CYRA_SPEC");
  if (path == nullptr || *path == '\0') return Spec{};
  return load(path);
}

const Stage * Spec::find(const std::string & node, const std::string & group) const noexcept
{
  for (const auto & st : stages_) {
    if (st.node == node && st.group == group) return &st;
  }
  return nullptr;
}

const Stage & Spec::stage(const std::string & node, const std::string & group) const
{
  const Stage * st = find(node, group);
  if (st == nullptr) {
    fail(
      "no stage '" + detail::stage_name(node, group) +
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
struct Claims
{
  std::mutex m;
  std::map<std::string, int> tid_of;
};
Claims & claims()
{
  static Claims * c = new Claims;
  return *c;
}

// The stage this thread holds, released when the thread exits so a respawned thread can take it.
struct Held
{
  std::string name;
  ~Held()
  {
    if (name.empty()) return;
    std::lock_guard<std::mutex> lock(claims().m);
    claims().tid_of.erase(name);
  }
};
thread_local Held held;

}  // namespace

// One stage names one thread. Two threads under one declaration would leave the file describing
// neither, so the second is refused while the first is alive.
Report apply_checked(const Stage & stage)
{
  detail::reject_external(stage, "apply_checked()");
  const std::string name = detail::stage_name(stage.node, stage.group);
  Claims & c = claims();
  {
    std::lock_guard<std::mutex> lock(c.m);
    auto it = c.tid_of.find(name);
    if (it != c.tid_of.end() && name != held.name) {
      throw std::invalid_argument(
        "cyra: stage " + name + " is already applied on thread " + std::to_string(it->second) +
        "; one stage declares one thread");
    }
    if (!held.name.empty()) c.tid_of.erase(held.name);
    c.tid_of[name] = this_tid();
    held.name = name;
  }
  try {
    return apply_checked(stage.opts, stage.strict, stage.control_priority);
  } catch (...) {
    std::lock_guard<std::mutex> lock(c.m);
    c.tid_of.erase(name);
    held.name.clear();
    throw;
  }
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

bool explains(const Stage & stage, const ThreadState & st)
{
  if (stage.external) return stage.expect_priority > 0 && stage.expect_priority == st.priority;
  if (sched_policy(stage.opts.policy) != st.policy || stage.opts.priority != st.priority) {
    return false;
  }
  return stage.opts.cpus.empty() || within(st.cpus, stage.opts.cpus);
}

}  // namespace

Report verify(const Stage & stage, int tid)
{
  const std::string where = detail::stage_name(stage.node, stage.group);
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

  // What the file claims depends on which side owns the thread. An external stage says only what
  // priority it should already be at, so a claim about policy has to be derived from that: a
  // nonzero RT priority is only reachable under an RT policy.
  const bool rt_expected =
    stage.external ? stage.expect_priority > 0
                   : (stage.opts.policy == Policy::Fifo || stage.opts.policy == Policy::RoundRobin);
  const int want_priority = stage.external ? stage.expect_priority : stage.opts.priority;
  const bool observed_rt = st.policy == SCHED_FIFO || st.policy == SCHED_RR;

  if (stage.external) {
    if (!rt_expected) {
      add(
        rep, "observed-policy", Verdict::Unknown,
        "external stage " + where +
          " declares no expect_priority, so the file claims nothing "
          "about its policy; it runs " +
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

  // Affinity is checked only where cyra set it. An external stage's cpus are not in the file at
  // all, and a stage that asked for no pin is free to run anywhere by its own declaration.
  // The claim is observed within declared, the same reading apply_checked confirms: a cpuset
  // may narrow the mask, and a thread on fewer of its declared cpus has left none of them.
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
  return rep;
}

Report verify_process(const Spec & spec)
{
  Report rep;
  DIR * d = ::opendir("/proc/self/task");
  int found = 0;
  while (const dirent * e = ::readdir(d)) {
    if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
    const int tid = std::atoi(e->d_name);
    ThreadState st;
    try {
      st = observe(tid);
    } catch (const std::exception &) {
      continue;  // a thread that ended between the readdir and the read is not a finding
    }
    if (st.policy != SCHED_FIFO && st.policy != SCHED_RR) continue;
    const auto & stages = spec.stages();
    if (std::any_of(
          stages.begin(), stages.end(), [&](const Stage & s) { return explains(s, st); })) {
      continue;
    }
    ++found;
    add(
      rep, "undeclared-rt-thread", Verdict::Warn,
      "tid " + std::to_string(tid) + " runs " + sched_name(st.policy) + " at priority " +
        std::to_string(st.priority) + " on cpus " + cpu_list(st.cpus) +
        ", which no stage of the file declares");
  }
  ::closedir(d);

  if (found == 0) {
    add(
      rep, "undeclared-rt-thread", Verdict::Ok,
      "every RT thread of this process runs at a declared stage's scheduling");
  }
  return rep;
}

}  // namespace cyra
