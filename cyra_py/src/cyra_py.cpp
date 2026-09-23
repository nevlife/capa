// cyra Python binding (nanobind): the applying half of cyra_core and the chain declaration file.

#include "cyra/rt.hpp"
#include "cyra/spec.hpp"

#include <nanobind/nanobind.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <cstdint>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace nb = nanobind;

NB_MODULE(_cyra, m)
{
  m.doc() =
    "cyra: OS scheduling for the calling thread and the chain declaration file. Not a real-time "
    "guarantee from Python: GIL and GC stay unbounded latency sources whatever policy a thread "
    "holds. What this settles is which thread the kernel prefers and which cores it may use.";

  // A kernel refusal carries an errno, and errno failures are OSError in Python. Without this
  // nanobind lands them on RuntimeError, where `except OSError` does not catch them.
  nb::register_exception_translator(
    [](const std::exception_ptr & p, void *) {
      try {
        std::rethrow_exception(p);
      } catch (const std::system_error & e) {
        PyErr_SetString(PyExc_OSError, e.what());
      }
    },
    nullptr);

  // Bound rather than rewritten in ctypes. The rollback on a half-applied request and the
  // read-back that catches an affinity a cpuset narrowed are the parts that are easy to get wrong
  // and easy to leave out; a second implementation is a second place for them to drift.
  //
  // Only the applying half is here. The value form of `apply` runs preflight at Soft strictness,
  // so a request the kernel cannot satisfy is refused with the reason, while a host that merely
  // cannot hold a deadline is reported and applied. A stage carries its chain's strictness.

  nb::enum_<cyra::Policy>(m, "Policy")
    .value("Inherit", cyra::Policy::Inherit, "Leave the scheduling class alone. The default.")
    .value("Other", cyra::Policy::Other, "SCHED_OTHER: explicit return to the default class.")
    .value(
      "Fifo", cyra::Policy::Fifo,
      "SCHED_FIFO: static priority, runs until it blocks or is preempted. Needs RLIMIT_RTPRIO or "
      "CAP_SYS_NICE; without either this is refused, never downgraded.")
    .value(
      "RoundRobin", cyra::Policy::RoundRobin,
      "SCHED_RR: Fifo plus a timeslice among equal priorities. Same permission.");

  nb::class_<cyra::ThreadState>(m, "ThreadState", "What a thread actually runs at.")
    .def_ro(
      "policy", &cyra::ThreadState::policy,
      "The raw SCHED_* number, not Policy: a thread may hold a class this enum does not name.")
    .def_ro("priority", &cyra::ThreadState::priority)
    .def_ro("cpus", &cyra::ThreadState::cpus, "The cores this thread may run on.")
    .def("__repr__", [](const cyra::ThreadState & st) {
      std::string cpus;
      for (auto c : st.cpus) cpus += (cpus.empty() ? "" : ",") + std::to_string(c);
      return "<cyra.ThreadState policy=" + std::to_string(st.policy) +
             " priority=" + std::to_string(st.priority) + " cpus=[" + cpus + "]>";
    });

  m.def(
    "apply",
    [](cyra::Policy policy, int priority, std::vector<std::uint32_t> cpus) {
      cyra::Options opts{policy, priority, std::move(cpus)};
      return cyra::apply_checked(opts, cyra::Strictness::Soft).to_string();
    },
    nb::arg("policy") = cyra::Policy::Inherit, nb::arg("priority") = 0,
    nb::arg("cpus") = std::vector<std::uint32_t>{},
    "Put the CALLING thread on this policy, priority and cpu set, and confirm by reading back "
    "what the kernel gave. A thread can only do this to itself, which is why an executor's "
    "children apply their own.\n\n"
    "Raises rather than settling for less: OSError when the kernel refuses, RuntimeError when a "
    "check finds the request cannot take effect on this host or when the read-back disagrees "
    "with what was asked. Asking for nothing does nothing. Returns a human-readable report of "
    "what the host was found to be. Diagnostic text, not a format to parse; empty when nothing "
    "was asked.");

  m.def(
    "current", &cyra::current, "What the calling thread runs at right now, read from the kernel.");

  m.def(
    "this_tid", &cyra::this_tid,
    "The calling thread's kernel thread id, which is what `top -H` and /proc name it by. "
    "threading.get_ident() is not this number.");

  // The chain declaration file, read by the parser C++ uses. Only what applying a stage needs.
  nb::class_<cyra::Stage>(m, "Stage", "One thread of a chain, as the declaration file states it.")
    .def_ro("node", &cyra::Stage::node, "Fully-qualified node name.")
    .def_ro("group", &cyra::Stage::group, "Group label; empty means the node's default group.")
    .def_ro("chain", &cyra::Stage::chain, "The chain that declared this stage.")
    .def_ro("external", &cyra::Stage::external, "cyra does not set this thread.")
    .def_ro("expect_priority", &cyra::Stage::expect_priority)
    .def_prop_ro("policy", [](const cyra::Stage & st) { return st.opts.policy; })
    .def_prop_ro("priority", [](const cyra::Stage & st) { return st.opts.priority; })
    .def_prop_ro("cpus", [](const cyra::Stage & st) { return st.opts.cpus; })
    .def_ro("control_priority", &cyra::Stage::control_priority);

  nb::class_<cyra::Spec>(m, "Spec", "A parsed chain declaration file (docs/en/api.en.md).")
    .def_static(
      "load",
      [](std::optional<std::string> path) {
        return path ? cyra::Spec::load(*path) : cyra::Spec::load();
      },
      nb::arg("path") = nb::none(),
      "Parse `path`, or the file CYRA_SPEC names when it is None. Unset gives an empty spec. "
      "RuntimeError on any rule the file breaks.")
    .def("empty", &cyra::Spec::empty)
    .def(
      "stage", &cyra::Spec::stage, nb::arg("node"), nb::arg("group") = "",
      nb::rv_policy::reference_internal,
      "The stage for this fully-qualified node name and group label. RuntimeError when the file "
      "does not name it.");

  m.def(
    "apply", [](const cyra::Stage & stage) { return cyra::apply_checked(stage).to_string(); },
    nb::arg("stage"),
    "Put the CALLING thread on a declared stage, with the chain's target and control-loop "
    "priority. Same read-back and refusals as the other form; ValueError for an external stage.");
}
