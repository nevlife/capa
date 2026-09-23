# cyra_example

Thread scheduling. C++ only. Python is not an RT target. The GIL and GC are unbounded sources of latency, so no bound can be promised there.

```bash
ros2 run cyra_example preflight

export CYRA_SPEC=$(ros2 pkg prefix cyra_example)/share/cyra_example/config/perception_to_control.yaml
ros2 run cyra_example chain
```

## preflight, ask before applying

`apply()` succeeds if the kernel accepts. The kernel readily accepts a host that cannot meet the deadline. `preflight()` judges, before touching anything, whether this host can sustain that schedule. The judgment is based entirely on what the kernel reports (rlimit, `/proc`, `/sys`), not on estimates.

```cpp
cyra::Options opts;
opts.policy = cyra::Policy::Fifo;
opts.priority = 80;
opts.cpus = {2};
const cyra::Report report = cyra::preflight(opts, 90);
```

Without `control_priority` the `priority-order` check comes out as `Unknown`. A check that did not run must not look the same as one that passed.

`Fail` means the request cannot hold as stated and blocks under both soft and hard. `Warn` means the request is accepted but the host is not a bounded latency configuration, so soft passes over it and hard refuses.

## chain, the declaration in one file

A chain crosses processes. No node can see on its own the order its configuration must respect. So the declaration is written in one file and each node looks up only its own share at startup. Nothing pushes, so it holds even when nodes are started and killed separately by launch.

```cpp
const cyra::Spec spec = cyra::Spec::load();   // CYRA_SPEC
const cyra::Stage & stage = spec.stage(node->get_fully_qualified_name(), "work");
cyra::flux::schedule(ex, *node, node->group(), stage);
```

`schedule` registers the executor's `on_thread_start` hook. The child thread that owns that group applies it to itself before the first callback. A thread can only apply to itself, so without this order there is a window where a callback runs once at the wrong priority.

The stage is passed whole. `strict` and `control_priority` are values no node can derive alone, and unpacking them into fields leaks them at that spot.

Without `CYRA_SPEC` the spec is empty and a no-op.

## Declaration file

It is `config/perception_to_control.yaml`.

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
```

- `stages` is the order the data flows. RT priority must increase in that direction. If upstream preempts downstream it starves it.
- `external` is a stage cyra does not apply. It is someone else's thread, such as an rmw listener. Only `expect_priority` may be written, and passing it to `schedule` is refused. All cyra does about it is `verify()`.
- Unknown keys are refused. There is no version field.
- With `target: hard` every RT stage must get its own core. Two pinned to the same core are refused. `SCHED_FIFO` has no timeslice, so one blocks the other completely.

`chain` hits `RLIMIT_RTPRIO` unless run as root. `preflight` says so first.
