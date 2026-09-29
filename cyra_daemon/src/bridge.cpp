#include "bridge.hpp"

#include "cyrad/src/ffi.rs.h"

#include <sched.h>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

namespace cyrad::bridge
{

namespace
{

rust::Vec<std::uint32_t> to_rust(const std::vector<std::uint32_t> & v)
{
  rust::Vec<std::uint32_t> out;
  out.reserve(v.size());
  for (const auto c : v) out.push_back(c);
  return out;
}

std::vector<std::uint32_t> to_std(rust::Slice<const std::uint32_t> s)
{
  return {s.begin(), s.end()};
}

std::string to_std(const rust::String & s)
{
  return std::string(s);
}

}  // namespace

std::unique_ptr<cyra::Spec> load(rust::Str path)
{
  return std::make_unique<cyra::Spec>(cyra::Spec::load(std::string(path)));
}

rust::Vec<Stage> stages(const cyra::Spec & spec)
{
  rust::Vec<Stage> out;
  out.reserve(spec.stages().size());
  for (const cyra::Stage & st : spec.stages()) {
    Stage s;
    s.key = cyra::detail::stage_name(st);
    s.node = st.node;
    s.group = st.group;
    s.name = st.name;
    s.external = st.external;
    s.matcher.irq = st.match.irq;
    s.matcher.process = st.match.process;
    s.matcher.thread = st.match.thread;
    s.cpus = to_rust(st.opts.cpus);
    s.children_cpus = to_rust(st.children_cpus);
    out.push_back(std::move(s));
  }
  return out;
}

rust::String refusal(const cyra::Spec & spec, std::size_t index, int tid)
{
  const cyra::Stage & st = spec.stages().at(index);
  const cyra::Report host = cyra::detail::preflight_to(tid, st.opts, 0);
  const bool warned = std::any_of(host.findings.begin(), host.findings.end(), [](const auto & f) {
    return f.verdict == cyra::Verdict::Warn;
  });
  if (host.ok() && !(st.strict == cyra::Strictness::Hard && warned)) return {};
  return host.to_string();
}

rust::String drift(const cyra::Spec & spec, std::size_t index, int tid)
{
  const cyra::Report now = cyra::verify(spec.stages().at(index), tid);
  if (now.ok()) return {};
  return now.to_string();
}

void apply(const cyra::Spec & spec, std::size_t index, int tid)
{
  const cyra::Stage & st = spec.stages().at(index);
  cyra::detail::apply_checked_to(tid, st.opts, st.strict, 0);
}

ThreadState observe(int tid)
{
  const cyra::ThreadState st = cyra::observe(tid);
  ThreadState out;
  out.policy = st.policy;
  out.priority = st.priority;
  out.cpus = to_rust(st.cpus);
  out.reset_on_fork = st.reset_on_fork;
  return out;
}

void put_back(int tid, const ThreadState & state)
{
  cyra::Options o;
  switch (state.policy) {
    case SCHED_OTHER:
      o.policy = cyra::Policy::Other;
      break;
    case SCHED_FIFO:
      o.policy = cyra::Policy::Fifo;
      o.priority = state.priority;
      break;
    case SCHED_RR:
      o.policy = cyra::Policy::RoundRobin;
      o.priority = state.priority;
      break;
    default:
      throw std::invalid_argument(
        "cyrad: policy " + std::to_string(state.policy) + " cannot be put back");
  }
  o.cpus.assign(state.cpus.begin(), state.cpus.end());
  const bool rt = o.policy == cyra::Policy::Fifo || o.policy == cyra::Policy::RoundRobin;
  o.children = rt && !state.reset_on_fork ? cyra::Children::Inherit : cyra::Children::Reset;
  cyra::detail::apply_to(tid, o);
}

void move_to(int tid, rust::Slice<const std::uint32_t> cpus)
{
  cyra::Options o;
  o.cpus = to_std(cpus);
  cyra::detail::apply_to(tid, o);
}

rust::Vec<Binding> bindings()
{
  rust::Vec<Binding> out;
  for (const cyra::Binding & b : cyra::bindings()) {
    out.push_back(Binding{b.node, b.group, b.name, b.pid, b.tid});
  }
  return out;
}

rust::Vec<HeldStage> held_stages()
{
  rust::Vec<HeldStage> out;
  for (const auto & h : cyra::detail::held_stages()) {
    HeldStage s;
    s.binding =
      Binding{h.binding.node, h.binding.group, h.binding.name, h.binding.pid, h.binding.tid};
    for (const auto & [tid, start] : h.earlier) s.earlier.push_back(Task{tid, start});
    out.push_back(std::move(s));
  }
  return out;
}

rust::String daemon_dir()
{
  return cyra::detail::daemon_dir();
}

rust::String record(rust::Slice<const Binding> held)
{
  std::vector<cyra::Binding> v;
  v.reserve(held.size());
  for (const Binding & b : held) {
    v.push_back({to_std(b.node), to_std(b.group), to_std(b.name), b.pid, b.tid});
  }
  return cyra::detail::record_bindings(v);
}

std::uint64_t task_starttime(int pid, int tid)
{
  return cyra::detail::task_starttime(pid, tid);
}

void bind(rust::Str target)
{
  cyra::bind(std::string(target));
}

int this_tid()
{
  return cyra::this_tid();
}

}  // namespace cyrad::bridge
