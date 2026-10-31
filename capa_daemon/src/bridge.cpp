#include "bridge.hpp"

#include "capad/src/ffi.rs.h"

#include <sched.h>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

namespace capad::bridge
{

namespace
{

auto to_rust(const std::vector<std::uint32_t> & v) -> rust::Vec<std::uint32_t>
{
  rust::Vec<std::uint32_t> out;
  out.reserve(v.size());
  for (const auto c : v) {
    out.push_back(c);
  }
  return out;
}

auto to_std(rust::Slice<const std::uint32_t> s) -> std::vector<std::uint32_t>
{
  return {s.begin(), s.end()};
}

auto to_std(const rust::String & s) -> std::string
{
  return std::string(s);
}

}  // namespace

auto load(rust::Str path) -> std::unique_ptr<capa::Spec>
{
  return std::make_unique<capa::Spec>(capa::Spec::load(std::string(path)));
}

auto stages(const capa::Spec & spec) -> rust::Vec<Stage>
{
  rust::Vec<Stage> out;
  out.reserve(spec.stages().size());
  for (const capa::Stage & st : spec.stages()) {
    Stage s;
    s.key = capa::detail::stage_name(st);
    s.node = st.node;
    s.group = st.group;
    s.name = st.name;
    s.external = st.external;
    s.matcher.irq = st.match.irq;
    s.matcher.process = st.match.process;
    s.matcher.arg = st.match.arg;
    s.matcher.thread = st.match.thread;
    s.cpus = to_rust(st.opts.cpus);
    s.children_cpus = to_rust(st.children_cpus);
    out.push_back(std::move(s));
  }
  return out;
}

auto refusal(const capa::Spec & spec, std::size_t index, int tid) -> rust::String
{
  const capa::Stage & st = spec.stages().at(index);
  const capa::Report host = capa::detail::preflight_to(tid, st.opts, 0, st.strict);
  const bool warned = std::any_of(host.findings.begin(), host.findings.end(), [](const auto & f) {
    return f.verdict == capa::Verdict::Warn;
  });
  if (host.ok() && !(st.strict == capa::Strictness::Hard && warned)) {
    return {};
  }
  return host.to_string();
}

auto drift(const capa::Spec & spec, std::size_t index, int tid) -> rust::String
{
  const capa::Report now = capa::verify(spec.stages().at(index), tid);
  if (now.ok()) {
    return {};
  }
  return now.to_string();
}

void apply(const capa::Spec & spec, std::size_t index, int tid)
{
  const capa::Stage & st = spec.stages().at(index);
  capa::detail::apply_checked_to(tid, st.opts, st.strict, 0);
}

auto observe(int tid) -> ThreadState
{
  const capa::ThreadState st = capa::observe(tid);
  ThreadState out;
  out.policy = st.policy;
  out.priority = st.priority;
  out.cpus = to_rust(st.cpus);
  out.reset_on_fork = st.reset_on_fork;
  return out;
}

void put_back(int tid, const ThreadState & state)
{
  capa::Options o;
  switch (state.policy) {
    case SCHED_OTHER:
      o.policy = capa::Policy::Other;
      break;
    case SCHED_FIFO:
      o.policy = capa::Policy::Fifo;
      o.priority = state.priority;
      break;
    case SCHED_RR:
      o.policy = capa::Policy::RoundRobin;
      o.priority = state.priority;
      break;
    default:
      throw std::invalid_argument(
        "capad: policy " + std::to_string(state.policy) + " cannot be put back");
  }
  o.cpus.assign(state.cpus.begin(), state.cpus.end());
  const bool rt = o.policy == capa::Policy::Fifo || o.policy == capa::Policy::RoundRobin;
  o.children = rt && !state.reset_on_fork ? capa::Children::Inherit : capa::Children::Reset;
  capa::detail::apply_to(tid, o);
}

void move_to(int tid, rust::Slice<const std::uint32_t> cpus)
{
  capa::Options o;
  o.cpus = to_std(cpus);
  capa::detail::apply_to(tid, o);
}

auto bindings() -> rust::Vec<Binding>
{
  rust::Vec<Binding> out;
  for (const capa::Binding & b : capa::bindings()) {
    out.push_back(Binding{b.node, b.group, b.name, b.pid, b.tid});
  }
  return out;
}

auto held_stages() -> rust::Vec<HeldStage>
{
  rust::Vec<HeldStage> out;
  for (const auto & h : capa::detail::held_stages()) {
    HeldStage s;
    s.binding =
      Binding{h.binding.node, h.binding.group, h.binding.name, h.binding.pid, h.binding.tid};
    for (const auto & [tid, start] : h.earlier) {
      s.earlier.push_back(Task{tid, start});
    }
    out.push_back(std::move(s));
  }
  return out;
}

auto daemon_dir() -> rust::String
{
  return capa::detail::daemon_dir();
}

auto record(rust::Slice<const Binding> held) -> rust::String
{
  std::vector<capa::Binding> v;
  v.reserve(held.size());
  for (const Binding & b : held) {
    v.push_back({to_std(b.node), to_std(b.group), to_std(b.name), b.pid, b.tid});
  }
  return capa::detail::record_bindings(v);
}

auto task_starttime(int pid, int tid) -> std::uint64_t
{
  return capa::detail::task_starttime(pid, tid);
}

void bind(rust::Str target)
{
  capa::bind(std::string(target));
}

auto this_tid() -> int
{
  return capa::this_tid();
}

}  // namespace capad::bridge
