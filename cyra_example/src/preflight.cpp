#include "cyra/rt.hpp"

#include <cstdio>
#include <cstdlib>

// Ask before applying anything whether this host can hold the schedule. Verdicts come from
// kernel-readable sources, so the answer is about this machine.
int main()
{
  cyra::Options opts;
  opts.policy = cyra::Policy::Fifo;
  opts.priority = 80;
  opts.cpus = {2};

  const int control_priority = 90;
  const cyra::Report report = cyra::preflight(opts, control_priority);
  std::printf("%s\n", report.to_string().c_str());
  return report.ok() ? EXIT_SUCCESS : EXIT_FAILURE;
}
