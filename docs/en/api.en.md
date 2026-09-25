# cyra user API

cyra sets and checks the OS scheduling of the threads on a dataflow path: policy, priority, and CPUs. It has three packages.

| Package | Contents | Depends on |
| --- | --- | --- |
| `cyra_core` | C++17, no ROS. Thread scheduling, host preflight, read-back, the chain declaration file | yaml-cpp |
| `cyra_py` | Python binding of `cyra_core` (`cyra`), and `cyra.flux` | `cyra_core` |
| `cyra_flux` | C++ scheduling for the threads of a flux `PartitionedExecutor` | `cyra_core`, `flux_cpp` |

## 1. Thread scheduling (C++)

```cpp
#include "cyra/rt.hpp"

cyra::Options o;
o.policy = cyra::Policy::Fifo;   // Inherit / Other / Fifo / RoundRobin
o.priority = 80;                 // 1..99 for Fifo/RoundRobin
o.cpus = {3};                    // leave empty to keep affinity untouched

cyra::Report rep = cyra::preflight(o, /*control_priority=*/90);  // judgment only, no change
cyra::apply(o);                  // applies to the calling thread. throws if refused
cyra::ThreadState st = cyra::current();                 // read back from the kernel
cyra::ThreadState other = cyra::observe(cyra::this_tid());  // the same values for another thread

rep = cyra::apply_checked(o, cyra::Strictness::Hard, /*control_priority=*/90);
```

- `apply` applies to the calling thread itself. It is all-or-nothing. If the kernel refuses, it reverts and throws `std::system_error`. There is no silent downgrade.
- `preflight` changes nothing and reports why it would not work as a list of findings (`rep.ok()`, `rep.to_string()`).
- `apply_checked` joins the two. It runs `preflight` first, and if the judgment blocks, it throws `std::runtime_error` before touching the thread. The kernel accepts the request even on a host that cannot meet deadlines, so with `apply` alone, "success" does not mean "runs in real time".
- `Strictness` decides how `Warn` is read. `Soft` accepts it and returns it in the report. `Hard` refuses it. `Fail` is refused by both. The default is `Soft`.
- An `Options` that requests nothing is a complete no-op even in `apply_checked`. There is nothing to judge.
- `current` reads your own thread from the kernel and `observe(tid)` reads another thread. Applying works only on your own thread, but reading works even across processes. `tid` comes from `this_tid()`. `std::thread::id` is not a name the kernel knows. A thread that cannot be read is a `std::system_error`.

To look at the items directly instead of the one-line summary, iterate `rep.findings` or pick one with `rep.find(id)`.

```cpp
for (const cyra::Finding & f : rep.findings) {
  if (f.verdict == cyra::Verdict::Fail) {
    log(f.id, f.detail);
  }
}
const cyra::Finding * one = rep.find("cpu-online");   // nullptr if not checked
```

`Finding` has three parts: `id` (a stable slug), `verdict`, and `detail` (why that verdict). `Verdict` is `Ok`, `Warn`, `Fail`, or `Unknown`, and `ok()` means "no `Fail`". `Unknown` means the kernel did not expose the source, not a pass.

| finding id | Source | Meaning of Fail/Warn |
| --- | --- | --- |
| `policy-permission` | `CapEff`(CAP_SYS_NICE) + `RLIMIT_RTPRIO` | Fail: no permission. `apply` throws with EPERM |
| `rt-throttle` | `/proc/sys/kernel/sched_rt_runtime_us` | Warn: the throttle preempts a busy RT thread regardless of priority |
| `priority-order` | opts.priority vs `control_priority` | Fail: transport is at or above control. Transport must never preempt control |
| `cpu-online` | `/sys/devices/system/cpu/online` | Fail: nonexistent core. `apply` throws with EINVAL |
| `cpu-isolation` | `/sys/devices/system/cpu/isolated`·`nohz_full` | Warn: pinning only removes migration. A core without isolation is shared with others |
| `rcu-offload` | `rcu_nocbs` in `/proc/cmdline` + `/sys/devices/system/cpu/nohz_full` | Warn: deferred kernel frees run on the RT core. Neither timing nor duration is bounded |
| `irq-affinity` | `/proc/irq/*/effective_affinity_list` | Warn: handlers preempt every task on that core regardless of RT priority |
| `cpu-governor` | `cpufreq/scaling_governor` | Warn: the DVFS ramp lengthens wakeup latency. RT cores use performance |
| `kernel-preemption` | `/sys/kernel/realtime`·`/proc/version` | Warn: not PREEMPT_RT. No latency bound for in-kernel sections |
| `memory-lock` | `VmLck` in `/proc/self/status` | Warn: no locked memory. A page fault is an unbounded delay |

## 2. Chain declaration file (C++)

Write the scheduling of one chain in one file, and each node finds and applies its own part when it starts. It holds even when nodes are started and killed individually by launch. Nothing pushes, and each reads on its own.

```yaml
chains:
  perception_to_control:
    target: hard                # blocks Warn. soft only records it
    stages:
      - node: /camera_node
        group: dds_listener
        external: true          # cyra does not apply it. someone else's thread, such as an rmw listener
        expect_priority: 60
      - node: /camera_node      # group omitted = the node's default callback group
        policy: fifo
        priority: 70
        cpus: [2]
      - node: /perception_node
        group: infer
        policy: fifo
        priority: 75
      - node: /control_node
        group: loop
        policy: fifo
        priority: 90
```

```cpp
#include "cyra/spec.hpp"

cyra::Spec spec = cyra::Spec::load();   // CYRA_SPEC. empty spec if absent
const std::string name = node->get_fully_qualified_name();
if (!spec.empty()) {
  std::thread worker([&spec, &name] {   // a thread I created applies it to itself
    cyra::apply_checked(spec.stage(name, "infer_worker"));
    cyra::verify(spec.stage(name, "infer_worker"), cyra::this_tid());
  });
  worker.join();
}
for (const cyra::Stage * e : spec.external()) {   // stages cyra does not apply
  log(e->node, cyra::verify(*e, foreign_tid).to_string());   // verify, not only record
}
```

`cyra::Stage` holds everything the file knows about that stage: `node`, `group`, `external`, `expect_priority`, `opts`, `strict`, `control_priority`, and which chain declared it (`chain`). `cyra::Spec` gives all of them with `stages()`, one with `find()`, and only the ones cyra does not apply with `external()`. Python reads the file through the same parser (section 4).

- `stages` is the order in which data flows. RT priority must increase in that direction. An upstream stage that preempts a downstream one starves it. Non-RT stages such as `policy: other` are outside this rule.
- `control_priority` is derived by the file. The last RT priority in the chain is control, and that stage itself gets 0.
- Unknown keys are refused. There is no version field.
- One stage appearing in two chains is normal. If the two chains declare it differently, it is refused. Its `strict` is the strictest of those chains: a stage on a hard chain is hard whichever chain the file lists first.
- A hard chain gives each RT stage its own core. If two are pinned to the same core, it is refused. `SCHED_FIFO` has no timeslice, so one blocks the other completely. A soft chain allows it.
- An `external` stage carries only `expect_priority`, 1..99 or 0 for undeclared. It cannot have `policy`/`priority`/`cpus`, and every entry point that applies a stage refuses it.
- If `CYRA_SPEC` is unset or empty, the spec is empty. That is the same no-op as declaring nothing.
- `stage(node, group)` takes the node's fully-qualified name (`get_fully_qualified_name()`). Looking up a label not in the file throws.
- `apply_checked(stage)` puts the calling thread on the stage, with the chain's strictness and control priority. It takes the whole stage, because unpacking `strict` and `control_priority` into fields loses them. One stage is one thread: while one live thread of this process holds a stage, another thread applying it throws `std::invalid_argument`. The holder applying it again is allowed, and a thread that exits releases it.
- `verify(stage, tid)` applies nothing and only compares the declaration with reality. The findings are three: `observed-policy`, `observed-priority`, and `observed-cpus`. Items the file does not claim are `Unknown`. `observed-cpus` is Ok while the thread runs within the declared cpus, the same reading `apply_checked` confirms, so a cpuset that narrows the mask passes and a cpu outside the declaration fails. A thread that cannot be read is an `observed-thread` Fail. The caller supplies `tid`.
- `verify_process(spec)` asks the other question: is anything in this process running RT that the file never described. A thread is explained when it runs at some stage's declared policy, priority and cpus, which is also what a callback's fork-join child holds, so a child needs no declaration. Each thread nothing explains is one `undeclared-rt-thread` Warn naming its tid. When every RT thread is explained, one `undeclared-rt-thread` Ok says so. A Warn, not a Fail: cyra cannot know whether a thread it did not declare is a mistake.

## 3. flux PartitionedExecutor (C++, `cyra_flux`)

`flux::ros::PartitionedExecutor` gives each callback group its own thread. `cyra::flux::schedule` puts a declaration on that thread.

```cpp
#include "cyra/flux.hpp"

flux::ros::PartitionedExecutor ex;
ex.add_ros_node(node);
cyra::flux::schedule(ex, group, {cyra::Policy::Fifo, 90, {3}});   // values
cyra::flux::schedule(ex, *node, infer_group, spec.stage(name, "infer"));   // a stage of the file
ex.spin();
```

- The child thread serving the group applies the declaration to itself with `apply_checked` before its first callback. A refusal becomes an error from `spin()`. It does not fall back to a weaker setting.
- `schedule(ex, group, opts, strict, control_priority)` checks `opts` on the spot. `strict` defaults to `Soft` and `control_priority` to 0 (undeclared).
- `schedule(ex, node, group, stage)` checks the stage's name against the group on the spot. An rclcpp callback group has no name, so this pairing is the only link between the file's `(node, label)` and the thread. The group must belong to `node`, `node` must be the one the stage names, and an empty label means that node's default callback group. An `external` stage is refused.
- One declaration per group, and the group must be served by `ex`. Both are checked by the executor: a second declaration throws on the spot, and a declaration for a group no child serves is refused by `spin()`.
- Two groups given one stage are refused by `spin()`. The second child to apply it throws, since one stage is one thread (section 2).
- Call `spin()` from a `SCHED_OTHER` thread. Every child starts on the calling thread's scheduling, so from an RT caller a group with no declaration would run at that priority, above the declared groups. flux does not check this. `verify_process` reports such threads as `undeclared-rt-thread`.
- Put every input of one message_filters synchronizer in one group. The sync policy runs the matched callback under its own `std::mutex`, which has no priority inheritance, so inputs on two group threads tie the two groups' priorities together.
- The message_filters path allocates (policy queues and `shared_ptr` control blocks per frame). Keep it out of a hard chain. Plain flux delivery does not allocate.

## 4. Python (`cyra_py`)

`cyra` is a direct binding of the C++ core. Only the applying side is exposed. `preflight`, `Report`, and `Strictness` are the hard RT judgment machinery and are not in the Python surface.

It is not an RT guarantee. Whatever priority is set, the GIL and GC remain unbounded sources of delay. What it decides is which thread the kernel picks when several are runnable at once, and which core each uses. There is still a value to set, because threads actually run in parallel in sections that release the GIL. numpy, zlib, and decoding are those sections.

```python
import cyra

report = cyra.apply(cpus=[4])          # the calling thread
state = cyra.current()                 # read back from the kernel
klass, prio, where = state.policy, state.priority, state.cpus
tid = cyra.this_tid()                  # the number top -H and /proc use

spec = cyra.Spec.load()                # CYRA_SPEC. empty spec when unset
if not spec.empty():
    cyra.apply(spec.stage(node.get_fully_qualified_name(), "infer_worker"))
```

- `cyra.apply(policy=, priority=, cpus=)` applies to the calling thread and runs preflight at `Soft`. After applying, it reads back from the kernel and compares, so a request that did not land as is, because a cpuset narrowed the mask, does not pass as success. Kernel refusal is `OSError`, a request that does not hold on this host or a mismatched read-back is `RuntimeError`, and an out-of-range value is `ValueError`. If nothing is requested, nothing is done.
- `cyra.apply(stage)` puts the calling thread on a stage. A stage on a hard chain is applied at `Hard`, so a preflight Warn refuses it too. An external stage, or a stage another live thread holds, is `ValueError`.
- The string `apply` returns is the host judgment report. It is meant for people to read, not a format to parse. It is an empty string when nothing was requested.
- `policy` is one of `cyra.Policy`: `Inherit` (default, untouched), `Other` (SCHED_OTHER), `Fifo` (SCHED_FIFO), `RoundRobin` (SCHED_RR). `Fifo` and `RoundRobin` require `priority` 1..99 and `RLIMIT_RTPRIO` or `CAP_SYS_NICE`. Setting only affinity needs no permission.
- `cyra.current()` returns a `cyra.ThreadState`. `policy` is a raw `SCHED_*` integer, not a `Policy` (compare with `os.SCHED_FIFO`), `priority` is an integer, and `cpus` is a list of core numbers.
- `cyra.this_tid()` is the kernel thread id. It differs from the number `threading.get_ident()` gives.
- `cyra.Spec.load(path=None)` parses `path`, or the file `CYRA_SPEC` names. Unset gives an empty spec, and `empty()` says so. A rule the file breaks is `RuntimeError`. `spec.stage(node, group="")` takes the node's fully-qualified name, and a stage the file does not name is `RuntimeError`.
- `cyra.Stage` is read-only: `node`, `group`, `chain`, `external`, `expect_priority`, `policy`, `priority`, `cpus`, `control_priority`.

`cyra.flux` declares scheduling for the threads of a `flux.ros.PartitionedExecutor`.

```python
import cyra.flux

cyra.flux.set_thread_scheduling(ex, group, cpus=[4, 5])
cyra.flux.set_thread_scheduling(ex, node, policy=cyra.Policy.Fifo, priority=20)
cyra.flux.schedule(ex, infer_group, spec.stage(name, "infer"))
```

- The `unit` is either a callback group given to `add` or a node given to `add_ros_node`. In C++ it is one callback group, but rclpy splits the unit in two.
- The child applies the declaration to itself before its first callback, and a refusal becomes an exception from `spin()`. An external stage is refused there too.
- One declaration per unit, shared between the two functions. A declaration for a unit the executor does not run is refused by `spin()`.
- The stage's node is not checked against the unit. C++ checks it, but a flux group in Python does not know its node.
- `flux.ros.Executor` runs on the calling thread, so call `cyra.apply()` directly before its `spin()`.

## 5. The connection to flux

`cyra_flux` and `cyra.flux` link to flux directly and register the executor's `on_thread_start` hook (C++ `PartitionedExecutor::on_thread_start(group, fn)`, Python `PartitionedExecutor.on_thread_start(unit, fn)`). flux knows nothing about cyra. `cyra_core` and `cyra` do not depend on flux.

This connection method is not fixed. The hook, the direct link, and the package split may change at any time, and code outside `cyra_flux` and `cyra.flux` must not rely on them.
