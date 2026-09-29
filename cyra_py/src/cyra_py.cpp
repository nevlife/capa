// cyra Python binding (nanobind). Every name here is the cyra_core name of the same thing, and
// nothing here decides anything cyra_core does not: a second implementation would be a second
// place for the rules to drift. spawn() is the one exception and lives in cyra/__init__.py,
// because a Python function has to run on a thread Python started.

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
using namespace nb::literals;

namespace
{

std::string cpu_text(const std::vector<std::uint32_t> & cpus)
{
  std::string out;
  for (auto c : cpus) out += (out.empty() ? "" : ",") + std::to_string(c);
  return "[" + out + "]";
}

}  // namespace

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

  nb::enum_<cyra::Policy>(m, "Policy")
    .value("Inherit", cyra::Policy::Inherit)
    .value("Other", cyra::Policy::Other)
    .value("Fifo", cyra::Policy::Fifo)
    .value("RoundRobin", cyra::Policy::RoundRobin);

  nb::enum_<cyra::Children>(m, "Children")
    .value("Reset", cyra::Children::Reset)
    .value("Inherit", cyra::Children::Inherit);

  nb::enum_<cyra::Strictness>(m, "Strictness")
    .value("Soft", cyra::Strictness::Soft)
    .value("Hard", cyra::Strictness::Hard);

  nb::enum_<cyra::Verdict>(m, "Verdict")
    .value("Ok", cyra::Verdict::Ok)
    .value("Warn", cyra::Verdict::Warn)
    .value("Fail", cyra::Verdict::Fail)
    .value("Unknown", cyra::Verdict::Unknown);

  nb::enum_<cyra::Kind>(m, "Kind")
    .value("Worker", cyra::Kind::Worker)
    .value("Release", cyra::Kind::Release);

  nb::class_<cyra::Options>(m, "Options")
    .def(
      "__init__",
      [](
        cyra::Options * self, cyra::Policy policy, int priority, std::vector<std::uint32_t> cpus,
        cyra::Children children) {
        new (self) cyra::Options{policy, priority, std::move(cpus), children};
      },
      "policy"_a = cyra::Policy::Inherit, "priority"_a = 0, "cpus"_a = std::vector<std::uint32_t>{},
      "children"_a = cyra::Children::Reset)
    .def_rw("policy", &cyra::Options::policy)
    .def_rw("priority", &cyra::Options::priority)
    .def_rw("cpus", &cyra::Options::cpus)
    .def_rw("children", &cyra::Options::children)
    .def("validate", &cyra::Options::validate);

  nb::class_<cyra::ThreadState>(m, "ThreadState")
    .def_ro("policy", &cyra::ThreadState::policy, "The raw SCHED_* number; compare os.SCHED_FIFO.")
    .def_ro("priority", &cyra::ThreadState::priority)
    .def_ro("cpus", &cyra::ThreadState::cpus)
    .def_ro("reset_on_fork", &cyra::ThreadState::reset_on_fork)
    .def("__repr__", [](const cyra::ThreadState & st) {
      return "<cyra.ThreadState policy=" + std::to_string(st.policy) +
             " priority=" + std::to_string(st.priority) + " cpus=" + cpu_text(st.cpus) +
             " reset_on_fork=" + (st.reset_on_fork ? "True" : "False") + ">";
    });

  nb::class_<cyra::Finding>(m, "Finding")
    .def_ro("id", &cyra::Finding::id)
    .def_ro("verdict", &cyra::Finding::verdict)
    .def_ro("detail", &cyra::Finding::detail);

  nb::class_<cyra::Report>(m, "Report")
    .def_ro("findings", &cyra::Report::findings)
    .def("ok", &cyra::Report::ok)
    .def(
      "find", [](const cyra::Report & r, const std::string & id) { return r.find(id.c_str()); },
      "id"_a, nb::rv_policy::reference_internal)
    .def("to_string", &cyra::Report::to_string);

  nb::class_<cyra::Match>(m, "Match")
    .def_ro("irq", &cyra::Match::irq)
    .def_ro("process", &cyra::Match::process)
    .def_ro("thread", &cyra::Match::thread)
    .def("empty", &cyra::Match::empty);

  nb::class_<cyra::Stage>(m, "Stage")
    .def_ro("node", &cyra::Stage::node)
    .def_ro("group", &cyra::Stage::group)
    .def_ro("name", &cyra::Stage::name)
    .def_ro("kind", &cyra::Stage::kind)
    .def_ro("external", &cyra::Stage::external)
    .def_ro("expect_priority", &cyra::Stage::expect_priority)
    .def_ro("match", &cyra::Stage::match)
    .def_ro("children_cpus", &cyra::Stage::children_cpus)
    .def_ro("opts", &cyra::Stage::opts)
    .def_ro("strict", &cyra::Stage::strict)
    .def_ro("chain", &cyra::Stage::chain);

  nb::class_<cyra::Spec>(m, "Spec")
    .def_static(
      "load",
      [](std::optional<std::string> path) {
        return path ? cyra::Spec::load(*path) : cyra::Spec::load();
      },
      "path"_a = nb::none())
    .def("empty", &cyra::Spec::empty)
    .def("stages", &cyra::Spec::stages, nb::rv_policy::reference_internal)
    .def("stage", &cyra::Spec::stage, "target"_a, "group"_a = "", nb::rv_policy::reference_internal)
    .def("find", &cyra::Spec::find, "target"_a, "group"_a = "", nb::rv_policy::reference_internal)
    .def("external", &cyra::Spec::external, nb::rv_policy::reference_internal);

  m.def("preflight", &cyra::preflight, "opts"_a, "control_priority"_a = 0);
  m.def(
    "apply_checked",
    nb::overload_cast<const cyra::Options &, cyra::Strictness, int>(&cyra::apply_checked), "opts"_a,
    "strict"_a = cyra::Strictness::Soft, "control_priority"_a = 0);
  m.def("apply_checked", nb::overload_cast<const cyra::Stage &>(&cyra::apply_checked), "stage"_a);
  m.def("bind", &cyra::bind, "target"_a, "group"_a = "");
  m.def("current", &cyra::current);
  m.def("this_tid", &cyra::this_tid, "The kernel tid; threading.get_ident() is not this number.");
  m.def("observe", &cyra::observe, "tid"_a);
  m.def("verify", &cyra::verify, "stage"_a, "tid"_a);
  m.def("verify_process", nb::overload_cast<const cyra::Spec &>(&cyra::verify_process), "spec"_a);
  m.def(
    "verify_process", nb::overload_cast<const cyra::Spec &, int>(&cyra::verify_process), "spec"_a,
    "pid"_a);

  nb::class_<cyra::Binding>(m, "Binding")
    .def_ro("node", &cyra::Binding::node)
    .def_ro("group", &cyra::Binding::group)
    .def_ro("name", &cyra::Binding::name)
    .def_ro("pid", &cyra::Binding::pid)
    .def_ro("tid", &cyra::Binding::tid);
  m.def("bindings", &cyra::bindings);
}
