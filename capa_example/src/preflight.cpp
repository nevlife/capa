#include "capa/rt.hpp"

#include <cstdio>
#include <cstdlib>

// Ask before applying anything whether this host can hold the schedule. Verdicts come from
// kernel-readable sources, so the answer is about this machine.
auto main() -> int
{
  capa::Options opts;
  opts.policy = capa::Policy::Fifo;
  opts.priority = 80;
  opts.cpus = {2};

  const int control_priority = 90;
  const auto report = capa::preflight(opts, control_priority);
  if (!report) {
    std::fprintf(stderr, "%s\n", report.error().what.c_str());
    return EXIT_FAILURE;
  }
  std::printf("%s\n", report->to_string().c_str());
  return report->ok() ? EXIT_SUCCESS : EXIT_FAILURE;
}
