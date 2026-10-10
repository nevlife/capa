# cyra

Thread scheduling for ROS 2 nodes on a dataflow path. One file declares how every thread on a chain runs: policy, priority, CPUs, and what its children inherit. The file is checked as a whole (downstream above upstream where they share a CPU, release threads above the work they release, more critical chains first), and each thread applies its own stage with one line, is checked against the host first, and is read back from the kernel after. The name stands for Chain Yield, Rank & Affinity.

```cpp
cyra::bind("/control_node", "loop");                     // this thread
auto t = cyra::spawn("/perception_node", "infer", fn);   // a new thread, set before fn runs
cyra::flux::bind(ex, *node, group, "grab");              // a flux or flux2 PartitionedExecutor group
```

| Package | Contents |
| --- | --- |
| `cyra_core` | C++17, no ROS. The chain file (`cyra::Spec`), `bind`, `spawn`, `apply_checked`, `preflight`, `verify`, `verify_process` |
| `cyra_flux` | The same functions for `flux::ros::PartitionedExecutor` and `flux2::PartitionedExecutor` threads |
| `cyra_py` | Python with the same names: `cyra` and `cyra.flux` |
| `cyra_cli` | The `cyra` command: `cyra check` and `cyra status`, daemonless and read-only ([`docs/en/cli.en.md`](docs/en/cli.en.md)) |
| `cyra_daemon` | `cyrad` (Rust, on `cyra_core` through a cxx bridge): sets the threads a file matches by name (interrupt, DDS, SDK threads) and moves reset children ([`docs/en/daemon.en.md`](docs/en/daemon.en.md)) |
| `cyra_example` | `preflight` and `chain`, C++ only |

The API is in [`docs/en/api.en.md`](docs/en/api.en.md) ([Korean](docs/ko/api.ko.md)). Measuring latency on a host is [`docs/en/latency.en.md`](docs/en/latency.en.md).

## Build

```bash
git submodule update --init          # thirdparty/nanobind
source /opt/ros/jazzy/setup.bash
cd <workspace>
colcon build --symlink-install
colcon test
```

`cyra_core` also builds without ROS: `cmake -S cyra_core -B build`, with `libyaml-cpp-dev` installed. `-DCYRA_FUZZER=ON -DCMAKE_CXX_COMPILER=clang++` builds `fuzz_spec` for libFuzzer; without it, ctest runs the same harness over a fixed corpus.

## Checks

`pre-commit install` enables them. Per language:

| Language | Format | Lint |
| --- | --- | --- |
| C++ | clang-format | clang-tidy: defect classes, the C++ Core Guidelines and CERT, less the checks `.clang-tidy` names and says why (needs the build's `compile_commands.json`) |
| Rust (`cyra_daemon`) | rustfmt | clippy `pedantic` with `-D warnings` (also compiles the C++ half of the bridge), every `unsafe` block one operation with a `SAFETY:` comment; `unsafe` only in the bridge and the signal mask; cargo audit against the RustSec advisory database when `Cargo.lock` changes (needs `cargo install cargo-audit` and the network) |
| Python | ruff-format | flake8 (defect classes), mypy over `cyra` with the tracked stub `cyra/_cyra.pyi` |
| Markdown | markdownlint `--fix` | markdownlint |
| Shell | | shellcheck |
| YAML | | yamllint |

Across the tree: every public name is in both user documents (`scripts/check_api_docs.py`), the contract tables name tests that exist (`scripts/check_contracts.py`), and document fences equal the compiled example regions (`scripts/check_doc_examples.py`).

Preflight's judgments are tested on host snapshots in `cyra_core/test/hosts`: one captured from an x86 desktop, and written ones for Orin on JetPack 6, a container, and an RT kernel read as root. `capture.sh` there prints a snapshot of the running host, read-only.

`colcon test` also runs: C++ and Python names match (`test_surface.py`), the tracked stub matches the binding (`test_stub.py`), the yaml fences of the documents load, the fuzz corpus, and the three document checks above. `-DCYRA_SANITIZER=thread` (or `address,undefined`) builds `cyra_core` with a sanitizer.

## Connection to flux

`cyra_flux` and `cyra.flux` register the `on_thread_start` hook of a [flux](https://github.com/nevlife/flux) or flux2 `PartitionedExecutor`. They take the executor as a template parameter in C++ and as any object in Python, so neither links flux or flux2; their tests use both. flux and flux2 do not depend on cyra, and `cyra_core` depends on neither.

This connection method may change at any time. The hook, the direct link, and the package split are the current arrangement, not a contract.

## License

MIT
