#include "capa/spec.hpp"

#include <sys/mman.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>

// The declaration file is the one input capa reads that a person writes. Any text must end as a
// Spec or a std::runtime_error naming the problem; a crash, a hang or another exception type is
// the finding. Spec::load takes a path, so the bytes go through a memfd rather than the disk.

extern "C" auto LLVMFuzzerTestOneInput(const std::uint8_t * data, std::size_t size) -> int
{
  const int fd = ::memfd_create("capa_fuzz_spec", MFD_CLOEXEC);
  if (fd < 0) {
    std::abort();
  }
  if (size > 0 && ::write(fd, data, size) != static_cast<ssize_t>(size)) {
    std::abort();
  }
  const std::string path = "/proc/self/fd/" + std::to_string(fd);

  // An exception out of load() is the finding: a bad file is a returned error.
  const auto spec = capa::Spec::load(path);
  if (spec) {
    for (const capa::Stage & st : spec->stages()) {
      const std::string & target = st.name.empty() ? st.node : st.name;
      if (spec->find(target, st.group) == nullptr) {
        std::abort();  // every stage is found by its name
      }
    }
    (void)spec->external();
  }
  ::close(fd);
  return 0;
}
