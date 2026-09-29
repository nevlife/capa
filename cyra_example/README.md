# cyra_example

Thread scheduling. C++ only: Python is not an RT target, since the GIL and GC are unbounded sources of latency.

```bash
ros2 run cyra_example preflight

export CYRA_SPEC=$(ros2 pkg prefix cyra_example)/share/cyra_example/config/perception_to_control.yaml
ros2 run cyra_example chain
```

## preflight, ask before applying

The kernel accepts a request on a host that cannot hold a deadline. `preflight()` judges, before touching anything, whether this host can sustain the schedule. Every verdict comes from what the kernel reports (rlimit, `/proc`, `/sys`).

```cpp
cyra::Options opts;
opts.policy = cyra::Policy::Fifo;
opts.priority = 80;
opts.cpus = {2};
const cyra::Report report = cyra::preflight(opts, 90);
```

Without `control_priority` the `priority-order` check is `Unknown`. A check that did not run must not look like one that passed. `Fail` means the request cannot hold as stated and blocks under both soft and hard. `Warn` means the request takes effect but the host is not configured for bounded latency: soft reports it, hard refuses it.

## chain, one line per thread

A chain crosses processes, and no node can see the order its own settings must respect. The schedule lives in one file, and each node only says which stage its thread is.

```cpp
cyra::flux::bind(ex, *node, node->group(), "work");
```

The child thread serving the group applies the stage to itself before its first callback, and a refusal ends `spin()` with the reason. Without `CYRA_SPEC` nothing is registered.

The file is `config/perception_to_control.yaml`:

```yaml
chains:
  perception_to_control:
    target: soft
    stages:
      - node: /flux_loan_image_pub
        external: true
        expect_priority: 0
      - node: /cyra_stage
        group: work
        policy: fifo
        priority: 70
        cpus: [2]
      - node: /flux_control
        group: loop
        policy: fifo
        priority: 90
        cpus: [3]
```

- `external` is a thread cyra does not set. `verify()` is all cyra does for it.
- The rules for the file (flow, release, criticality, hard cores, children) are in `docs/en/api.en.md` section 2.

`chain` needs `RLIMIT_RTPRIO` of at least 70 unless run as root. `preflight` says so first.
