#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// Stand-in main() for builds without libFuzzer, so the harness stays compiled and run by ctest.
// Random bytes rarely get past the YAML reader, so the corpus starts from valid files and bends
// them: truncation, bit flips, numbers swapped for edge values, and stages duplicated.

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t * data, std::size_t size);

namespace
{

std::uint64_t next(std::uint64_t & s)
{
  s ^= s << 13;
  s ^= s >> 7;
  s ^= s << 17;
  return s;
}

void run(const std::string & text)
{
  // libFuzzer's entry point takes bytes.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  LLVMFuzzerTestOneInput(reinterpret_cast<const std::uint8_t *>(text.data()), text.size());
}

const char * const kSeeds[] = {
  "chains:\n  c:\n    target: hard\n    criticality: 2\n    stages:\n"
  "      - {name: irq, kind: release, policy: fifo, priority: 60, cpus: [2]}\n"
  "      - {node: /cam, group: grab, policy: fifo, priority: 55, cpus: [2], children: reset}\n"
  "      - {node: /per, group: infer, policy: fifo, priority: 70, cpus: [3], children: inherit}\n"
  "      - {node: /ctl, group: loop, policy: rr, priority: 80, cpus: [4]}\n"
  "  log:\n    target: soft\n    stages:\n"
  "      - {node: /cam, group: grab, policy: fifo, priority: 55, cpus: [2], children: reset}\n"
  "      - {node: /log, policy: other}\n",
  "chains:\n  e:\n    target: soft\n    stages:\n"
  "      - {node: /dds, group: listener, external: true, expect_priority: 40}\n"
  "      - {node: /w, policy: fifo, priority: 50}\n",
};

const char * const kEdges[] = {"-1", "0", "99", "100", "4294967296", "1e9", "~", "[]", "{}", "''"};

}  // namespace

int main(int argc, char ** argv)
{
  if (argc > 1) {
    for (int i = 1; i < argc; ++i) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): argv is a C array
      std::FILE * f = std::fopen(argv[i], "rb");
      if (f == nullptr) return 1;
      std::string text;
      char chunk[4096];
      std::size_t got = 0;
      while ((got = std::fread(chunk, 1, sizeof chunk, f)) > 0) text.append(chunk, got);
      std::fclose(f);
      run(text);
    }
    return 0;
  }

  std::uint64_t seed = 0x9E3779B97F4A7C15ull;
  for (const char * base : kSeeds) {
    const std::string s = base;
    run(s);
    for (std::size_t n = 0; n <= s.size(); n += 3) run(s.substr(0, n));
    for (int round = 0; round < 2000; ++round) {
      std::string m = s;
      for (int flip = 0; flip < 1 + static_cast<int>(next(seed) % 4); ++flip) {
        const unsigned bit = 1u << (next(seed) % 7);
        char & c = m[next(seed) % m.size()];
        c = static_cast<char>(c ^ bit);
      }
      run(m);
    }
    for (int round = 0; round < 2000; ++round) {
      std::string m = s;
      for (std::size_t at = m.find_first_of("0123456789"); at != std::string::npos;
           at = m.find_first_of("0123456789", at + 1)) {
        if (next(seed) % 4 != 0) continue;
        std::size_t end = m.find_first_not_of("0123456789", at);
        const std::string edge = kEdges[next(seed) % (sizeof kEdges / sizeof *kEdges)];
        m.replace(at, end - at, edge);
        at += edge.size();
      }
      run(m);
    }
    const std::size_t first = s.find("      - ");
    const std::size_t line_end = s.find('\n', first);
    std::string dup = s;
    for (int copies = 0; copies < 64; ++copies) {
      dup.insert(first, s.substr(first, line_end - first + 1));
    }
    run(dup);
  }
  return 0;
}
