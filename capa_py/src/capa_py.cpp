// capa Python binding (nanobind). Every name here is the capa_core name of the same thing, and
// nothing here decides anything capa_core does not: a second implementation would be a second
// place for the rules to drift. spawn() is the one exception and lives in capa/__init__.py,
// because a Python function has to run on a thread Python started.

#include "capa/rt.hpp"
#include "capa/spec.hpp"

#include <nanobind/nanobind.h>
#include <nanobind/stl/chrono.h>
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

NB_MODULE(_capa, m)
{
  m.doc() =
    "capa: OS scheduling for the calling thread and the chain declaration file. Not a real-time "
    "guarantee from Python: GIL and GC stay unbounded latency sources whatever policy a thread "
    "holds. What this settles is which thread the kernel prefers and which cores it may use.";

  // A kernel refusal carries an errno, and errno failures are OSError in Python. Without this
  // nanobind lands them on RuntimeError, where `except OSError` does not catch them.
  nb::register_exception_translator(
    [](const std::exception_ptr & p, void *) {
      try {
        std::rethrow_exception(p);
      } catch (const std::system_error & e) {
        PyObject * args = Py_BuildValue("(is)", e.code().value(), e.what());
        PyErr_SetObject(PyExc_OSError, args);
        Py_XDECREF(args);
      }
    },
    nullptr);

  nb::enum_<capa::Policy>(m, "Policy")
    .value("Inherit", capa::Policy::Inherit)
    .value("Other", capa::Policy::Other)
    .value("Fifo", capa::Policy::Fifo)
    .value("RoundRobin", capa::Policy::RoundRobin);

  nb::enum_<capa::Children>(m, "Children")
    .value("Reset", capa::Children::Reset)
    .value("Inherit", capa::Children::Inherit);

  nb::enum_<capa::Strictness>(m, "Strictness")
    .value("Soft", capa::Strictness::Soft)
    .value("Hard", capa::Strictness::Hard);

  nb::enum_<capa::Verdict>(m, "Verdict")
    .value("Ok", capa::Verdict::Ok)
    .value("Warn", capa::Verdict::Warn)
    .value("Fail", capa::Verdict::Fail)
    .value("Unknown", capa::Verdict::Unknown)
    .value("Info", capa::Verdict::Info);

  nb::enum_<capa::Kind>(m, "Kind")
    .value("Worker", capa::Kind::Worker)
    .value("Release", capa::Kind::Release);

  nb::class_<capa::Options>(m, "Options")
    .def(
      "__init__",
      [](
        capa::Options * self, capa::Policy policy, int priority, std::vector<std::uint32_t> cpus,
        capa::Children children) {
        new (self) capa::Options{policy, priority, std::move(cpus), children};
      },
      "policy"_a = capa::Policy::Inherit, "priority"_a = 0, "cpus"_a = std::vector<std::uint32_t>{},
      "children"_a = capa::Children::Reset)
    .def_rw("policy", &capa::Options::policy)
    .def_rw("priority", &capa::Options::priority)
    .def_rw("cpus", &capa::Options::cpus)
    .def_rw("children", &capa::Options::children)
    .def("validate", &capa::Options::validate);

  nb::class_<capa::ThreadState>(m, "ThreadState")
    .def_ro("policy", &capa::ThreadState::policy, "The raw SCHED_* number; compare os.SCHED_FIFO.")
    .def_ro("priority", &capa::ThreadState::priority)
    .def_ro("cpus", &capa::ThreadState::cpus)
    .def_ro("reset_on_fork", &capa::ThreadState::reset_on_fork)
    .def("__repr__", [](const capa::ThreadState & st) {
      return "<capa.ThreadState policy=" + std::to_string(st.policy) +
             " priority=" + std::to_string(st.priority) + " cpus=" + cpu_text(st.cpus) +
             " reset_on_fork=" + (st.reset_on_fork ? "True" : "False") + ">";
    });

  nb::class_<capa::Finding>(m, "Finding")
    .def_ro("id", &capa::Finding::id)
    .def_ro("verdict", &capa::Finding::verdict)
    .def_ro("detail", &capa::Finding::detail);

  nb::class_<capa::Report>(m, "Report")
    .def_ro("findings", &capa::Report::findings)
    .def("ok", &capa::Report::ok)
    .def(
      "find", [](const capa::Report & r, const std::string & id) { return r.find(id.c_str()); },
      "id"_a, nb::rv_policy::reference_internal)
    .def("to_string", &capa::Report::to_string);

  nb::class_<capa::Match>(m, "Match")
    .def_ro("irq", &capa::Match::irq)
    .def_ro("process", &capa::Match::process)
    .def_ro("thread", &capa::Match::thread)
    .def("empty", &capa::Match::empty);

  nb::class_<capa::Stage>(m, "Stage")
    .def_ro("node", &capa::Stage::node)
    .def_ro("group", &capa::Stage::group)
    .def_ro("name", &capa::Stage::name)
    .def_ro("kind", &capa::Stage::kind)
    .def_ro("external", &capa::Stage::external)
    .def_ro("expect_priority", &capa::Stage::expect_priority)
    .def_ro("match", &capa::Stage::match)
    .def_ro("children_cpus", &capa::Stage::children_cpus)
    .def_ro("opts", &capa::Stage::opts)
    .def_ro("strict", &capa::Stage::strict)
    .def_ro("chain", &capa::Stage::chain);

  nb::class_<capa::Spec>(m, "Spec")
    .def_static(
      "load",
      [](std::optional<std::string> path) {
        return path ? capa::Spec::load(*path) : capa::Spec::load();
      },
      "path"_a = nb::none())
    .def("empty", &capa::Spec::empty)
    .def("stages", &capa::Spec::stages, nb::rv_policy::reference_internal)
    .def("stage", &capa::Spec::stage, "target"_a, "group"_a = "", nb::rv_policy::reference_internal)
    .def("find", &capa::Spec::find, "target"_a, "group"_a = "", nb::rv_policy::reference_internal)
    .def("external", &capa::Spec::external, nb::rv_policy::reference_internal);

  m.def(
    "preflight", nb::overload_cast<const capa::Options &, int, capa::Strictness>(&capa::preflight),
    "opts"_a, "control_priority"_a = 0, "strict"_a = capa::Strictness::Soft);
  m.def("preflight", nb::overload_cast<const capa::Spec &>(&capa::preflight), "spec"_a);
  m.def(
    "apply_checked",
    nb::overload_cast<const capa::Options &, capa::Strictness, int>(&capa::apply_checked), "opts"_a,
    "strict"_a = capa::Strictness::Soft, "control_priority"_a = 0);
  m.def("apply_checked", nb::overload_cast<const capa::Stage &>(&capa::apply_checked), "stage"_a);
  m.def("bind", &capa::bind, "target"_a, "group"_a = "");
  m.def("current", &capa::current);
  m.def("this_tid", &capa::this_tid, "The kernel tid; threading.get_ident() is not this number.");
  m.def("observe", &capa::observe, "tid"_a);
  m.def(
    "verify", &capa::verify, "stage"_a, "tid"_a, "window"_a = std::chrono::milliseconds{0},
    nb::call_guard<nb::gil_scoped_release>());
  m.def("verify_process", nb::overload_cast<const capa::Spec &>(&capa::verify_process), "spec"_a);
  m.def(
    "verify_process", nb::overload_cast<const capa::Spec &, int>(&capa::verify_process), "spec"_a,
    "pid"_a);

  nb::class_<capa::Binding>(m, "Binding")
    .def_ro("node", &capa::Binding::node)
    .def_ro("group", &capa::Binding::group)
    .def_ro("name", &capa::Binding::name)
    .def_ro("pid", &capa::Binding::pid)
    .def_ro("tid", &capa::Binding::tid);
  m.def("bindings", &capa::bindings);
}
