# cyra

Thread scheduling for ROS 2 nodes on a dataflow path. cyra puts a thread on a policy, priority, and CPU set, judges beforehand whether the host can hold it, and reads back what the kernel actually gave. A chain declaration file states the scheduling of every thread on a path in one place, so the rule that RT priority climbs along the flow can be checked across processes.

| Package | Contents |
| --- | --- |
| `cyra_core` | C++17, no ROS. `cyra::apply`, `preflight`, `apply_checked`, `observe`, and the chain declaration file (`cyra::Spec`, `verify`, `verify_process`) |
| `cyra_py` | Python binding (`cyra`), and `cyra.flux` for `flux.ros.PartitionedExecutor` threads |
| `cyra_flux` | C++ `cyra::flux::schedule` for `flux::ros::PartitionedExecutor` threads |
| `cyra_example` | `preflight` and `chain`, C++ only |

The API is in [`docs/en/api.en.md`](docs/en/api.en.md) ([Korean](docs/ko/api.ko.md)).

## Build

```bash
git submodule update --init          # thirdparty/nanobind
source /opt/ros/jazzy/setup.bash
cd <workspace>
colcon build --symlink-install
colcon test
```

`cyra_core` also builds without ROS: `cmake -S cyra_core -B build`, with `libyaml-cpp-dev` installed.

## Connection to flux

`cyra_flux` and `cyra.flux` link to [flux](https://github.com/nevlife/flux) directly and register its `PartitionedExecutor::on_thread_start` hook. flux does not depend on cyra, and `cyra_core` does not depend on flux.

This connection method may change at any time. The hook, the direct link, and the package split are the current arrangement, not a contract.

## License

MIT
