# cyra user API

cyra sets and checks the OS scheduling of the threads on a dataflow path: policy, priority, CPUs, and what their children inherit. The schedule lives in one file; code only says which stage a thread is.

| Package | Contents | Depends on |
| --- | --- | --- |
| `cyra_core` | C++17, no ROS. The chain file, applying, host preflight, read-back, verification | yaml-cpp |
| `cyra_flux` | The same functions for the threads of a flux or flux2 `PartitionedExecutor` | `cyra_core` |
| `cyra_py` | Python: `cyra` (the `cyra_core` names) and `cyra.flux` (the `cyra_flux` names) | `cyra_core` |

C++ and Python have the same names, the same arguments in the same order, and the same defaults. The differences are only where a language forces one, and are listed in section 7.

## 1. One line per thread

The file names every stage of a chain and how it runs. `CYRA_SPEC` names the file.

```yaml
chains:
  perception_to_control:
    target: hard
    stages:
      - {node: /camera_node, group: grab, policy: fifo, priority: 60, cpus: [2]}
      - {node: /perception_node, group: infer, policy: fifo, priority: 70, cpus: [3]}
      - {node: /control_node, group: loop, policy: fifo, priority: 80, cpus: [4]}
```

A thread then says which stage it is. Which call depends on who created the thread.

| Thread | C++ | Python |
| --- | --- | --- |
| One you start | `cyra::spawn("/perception_node", "infer", fn)` | `cyra.spawn("/perception_node", "infer", fn)` |
| One that already runs (main, a thread you own) | `cyra::bind("/control_node", "loop")` | `cyra.bind("/control_node", "loop")` |
| A flux or flux2 `PartitionedExecutor` group thread | `cyra::flux::bind(ex, *node, group, "grab")` | `cyra.flux.bind(ex, node, group, "grab")` |

- `bind(target, group="")` puts the calling thread on the stage, checks the host, and reads the result back from the kernel. `target` is a fully-qualified node name (`get_fully_qualified_name()`) with the group label, or a `name` stage's name with no group.
- `spawn(target, group, fn)` starts a thread, binds it, and only then runs `fn`. It returns once the thread is set. If the bind is refused, `fn` never runs and the refusal is thrown to the caller. Prefer it over a thread that calls `bind` itself: that thread runs unscheduled until it reaches the `bind` line.
- `cyra::flux::bind(ex, node, group, label="")` registers the bind on the executor. The child serving `group` binds itself before its first callback, and a refusal is the `spin()` error. The stage is looked up at the call, so a typo throws there.
- With `CYRA_SPEC` unset, all three do nothing (`bind` returns an empty report, `spawn` just runs `fn`, `flux::bind` registers nothing).
- A label the file does not name throws. Applying nothing would hide a typo.

## 2. The chain file

```yaml
chains:
  perception_to_control:
    target: hard                  # hard: preflight Warns refuse the apply. soft: they are reported
    criticality: 2                # higher comes first on a shared cpu. default 0
    stages:
      - name: camera_usb_irq      # not a ROS thread: named by `name`
        kind: release             # hands work on; runs above the workers it feeds
        external: true            # cyra does not set it; raised from the default 50 with chrt
        expect_priority: 85       # an external stage may land on any cpu, so above every worker
      - node: /camera_node
        group: grab               # omitted = the node's default callback group
        policy: fifo              # inherit / other / fifo / rr
        priority: 55
        cpus: [2]
        children: reset           # reset (default) / inherit
      - node: /perception_node
        group: infer
        policy: fifo
        priority: 70
        cpus: [3]
        children: inherit         # OpenMP team threads do this stage's work
      - node: /control_node
        group: loop
        policy: fifo
        priority: 80
        cpus: [4]
```

| Key | Where | Meaning |
| --- | --- | --- |
| `target` | chain | `hard` or `soft`. Required |
| `criticality` | chain | 0 or more, default 0 |
| `stages` | chain | The threads in the order data flows |
| `node`, `group` | stage | A ROS thread: fully-qualified node name and group label |
| `name` | stage | Any other thread. Must not start with `/` and takes no `group` |
| `kind` | stage | `worker` (default) computes. `release` only hands work on: an IRQ thread, a DDS receive thread, a timer thread |
| `policy`, `priority`, `cpus`, `children` | stage | What cyra applies (section 3) |
| `external`, `expect_priority` | stage | A thread cyra does not set. Only `expect_priority` (1..99, 0 undeclared) may be written. Every applying entry point refuses it; `verify` checks it |
| `match` | stage | `{irq: <handler>}` or `{process: <comm>, thread: <comm>}`: a thread no code of ours runs on, which the daemon `cyrad` finds and sets. `bind` and `apply_checked` refuse it (`docs/en/daemon.en.md`) |
| `children_cpus` | stage | Where `cyrad` moves the reset children of this stage. Needs `cpus`, `children: reset`, and no cpu in common with `cpus` |

Rules, checked when the file is read. Every rule reports the stages that break it.

| Rule | Checked |
| --- | --- |
| Flow | In one chain, a downstream worker runs at or above an upstream worker whenever the two can share a cpu. Equal is fine: FIFO does not preempt at equal priority. Stages on disjoint cpus never preempt each other and are not compared |
| Release | A release stage runs strictly above every worker of its chain it can share a cpu with. Below them, the work it hands on is released late |
| Criticality | On a shared cpu, every RT stage of a more critical chain runs strictly above every RT stage of a less critical one. A stage on several chains takes their highest criticality |
| Hard core | Where a hard chain is involved, two RT workers are not pinned to one cpu. SCHED_FIFO has no timeslice, so one would hold the core until it blocks. Release stages do not count |
| One declaration | A stage on two chains is declared the same way on both. Its strictness is the strictest of them |
| One name | No two stages are spelled alike. Node `/foo/bar` with its default group and node `/foo` with group `bar` are both `/foo/bar`, and messages, records and the daemon name a stage by its spelling |

- "Can share a cpu" means the `cpus` overlap. No `cpus`, and an external stage, can land anywhere, so they share every cpu.
- Stages that are not RT (`policy: other`, `inherit`, external with `expect_priority: 0`) take part in no rule.
- Unknown keys are refused. There is no version field: a version announces a schema change but does not catch `priorty`.
- `cyra::Spec::load(path)` reads a file and `cyra::Spec::load()` reads `CYRA_SPEC` (unset gives an empty spec, `empty()` is true). `stages()` lists every stage, `find(target, group)` returns one or null, `stage(target, group)` returns one or throws, `external()` lists the external ones.
- A `cyra::Stage` carries `match` (a `Match` with `irq`, `process`, `thread`, and `empty()`), `children_cpus`, `node`, `group`, `name`, `kind` (`Kind::Worker` or `Kind::Release`), `external`, `expect_priority`, `opts`, `strict`, and `chain` (which chain declared it first).

## 3. Applying

Everything here applies to the calling thread only. A thread is never set from outside, so there is no window where another thread has half-configured it.

The functions are in `cyra/spec.hpp`, which includes `cyra/rt.hpp`.

```cpp doc:apply
cyra::Options o;
o.policy = cyra::Policy::Fifo;       // Inherit / Other / Fifo / RoundRobin
o.priority = 70;                     // 1..99 for Fifo/RoundRobin, else 0
o.cpus = {3};                        // empty leaves affinity alone
o.children = cyra::Children::Reset;  // Reset / Inherit

cyra::Report rep = cyra::apply_checked(o, cyra::Strictness::Hard, /*control_priority=*/80);
rep = cyra::apply_checked(spec.stage("/perception_node", "infer"));  // a stage from a Spec
rep = cyra::bind("/perception_node", "infer");                       // the stage from CYRA_SPEC
```

- `apply_checked(opts, strict=Soft, control_priority=0)` runs `preflight` first. If the verdicts block, it throws `std::runtime_error` before touching the thread. It then applies, and reads back from the kernel: policy and priority must match, and the thread's cpus must lie within the requested ones (a cpuset may narrow the mask). A mismatch throws, with the thread already changed.
- `Strictness` decides how `Warn` is read. `Soft` returns it in the report. `Hard` refuses it. `Fail` is refused by both.
- `apply_checked(stage)` and `bind` take the stage whole, so the chain's strictness comes with it. Its report carries `priority-order` as Ok: the file checked the ordering at load.
- One stage is one thread. While one live thread of this process holds a stage, another thread applying it throws `std::invalid_argument`. The holder applying it again is fine, and the stage is released when the holder exits.
- An `Options` that asks for nothing does nothing, judgement included, and returns an empty report.
- `Options::validate()` refuses what would be reinterpreted: an RT policy without priority 1..99, a priority without an RT policy, `children = Inherit` without an RT policy, a cpu id beyond the kernel set size.
- The kernel refusing is `std::system_error`. There is no silent downgrade.

### Children

Linux gives a new thread its parent's policy, priority, and cpus, and `fork()` and `exec()` keep them. An RT thread that calls into a library leaks RT into every pool, logger, and `system()` the library starts.

| `children` | Children start as | For |
| --- | --- | --- |
| `Reset` (default) | SCHED_OTHER, priority 0 | Library pools, loggers, subprocesses: anything not modeled as this stage |
| `Inherit` | The parent's policy and priority | Fork-join helpers doing this stage's work, such as OpenMP team threads |

- `Reset` is `SCHED_RESET_ON_FORK`. It covers `pthread_create` as well as `fork`, and only takes effect with an RT policy.
- Affinity is inherited either way. The kernel has no flag against it, so a reset child still runs on the stage's cpus, only when the stage's thread sleeps.
- Once a thread is set with `Reset`, only `CAP_SYS_NICE` can clear the flag. The same thread cannot later switch to `Inherit` without it; the refusal says so.
- A long-lived helper that needs its own scheduling is its own stage: bind it inside the helper, or declare it `external`.

## 4. Checking

```cpp doc:check
cyra::Report host = cyra::preflight(o, /*control_priority=*/80);  // judges, changes nothing
cyra::ThreadState self = cyra::current();                          // this thread, from the kernel
int tid = cyra::this_tid();                                        // the kernel's id for it
cyra::ThreadState other = cyra::observe(tid);                      // any thread, any process
cyra::Report drift = cyra::verify(stage, tid);                     // a thread against its stage
cyra::Report stray = cyra::verify_process(spec);                   // RT threads nothing explains
```

A `Report` holds `findings`. Each `Finding` has `id` (a stable slug), `verdict`, and `detail`. `Verdict` is `Ok`, `Warn`, `Fail`, or `Unknown`, and `Unknown` means the source could not be read, not a pass. `ok()` is "no `Fail`". `find(id)` returns one finding or null. `to_string()` is for people to read, not to parse.

`preflight` findings:

| id | Source | Fail / Warn |
| --- | --- | --- |
| `policy-permission` | `CapEff` (CAP_SYS_NICE) and `RLIMIT_RTPRIO` | Fail: no permission |
| `rt-throttle` | `/proc/sys/kernel/sched_rt_runtime_us` | Warn: the throttle takes the cpu from a busy RT thread regardless of priority |
| `priority-order` | `opts.priority` against `control_priority` | Fail: at or above the control loop. Unknown when `control_priority` is 0 |
| `cpu-online` | `/sys/devices/system/cpu/online` | Fail: the cpu does not exist |
| `cpu-isolation` | `/sys/devices/system/cpu/isolated`, `nohz_full` | Warn: the cpu is shared with other tasks |
| `rcu-offload` | `rcu_nocbs` in `/proc/cmdline`, `nohz_full` | Warn: deferred kernel frees run on the RT cpu |
| `irq-affinity` | `/proc/irq/*/effective_affinity_list` | Warn: interrupt handlers preempt every task on the cpu |
| `cpu-governor` | `cpufreq/scaling_governor` | Warn: frequency ramp adds wakeup latency |
| `kernel-preemption` | `/sys/kernel/realtime`, `/proc/version` | Warn: not PREEMPT_RT |
| `memory-lock` | `VmLck` in `/proc/self/status` | Warn: no locked memory; a page fault is an unbounded stall |

`verify(stage, tid)` applies nothing. `chrt`, systemd, or a later apply can move a thread after cyra set it; this is how that is seen.

| id | Meaning |
| --- | --- |
| `observed-thread` | Fail: the thread cannot be read. A thread that is gone must not read as a thread at rest |
| `observed-policy` | The declared policy against the thread's |
| `observed-priority` | The declared priority (or `expect_priority`) against the thread's |
| `observed-children` | The declared `children` against the thread's `SCHED_RESET_ON_FORK` |
| `observed-cpus` | Ok while the thread's cpus lie within the declared ones |

Items the file does not claim are `Unknown`.

`verify_process(spec)` lists every RT thread of this process that no stage explains, one `undeclared-rt-thread` Warn per thread, or one Ok when there is none. A thread is explained when it holds a stage (`bind`, `spawn`, `apply_checked`), or when it runs at the declared scheduling of a stage with `children: inherit`. A thread that merely runs at a stage's values got there without cyra and is listed. It is a Warn, not a Fail: cyra cannot know whether an undeclared thread is a mistake.

### Other processes

A thread that holds a stage is recorded, so other processes can see it. Each process that binds keeps one record, `/dev/shm/cyra.<pid>.<starttime>` (`CYRA_RUN_DIR` moves it), write-locked for as long as it lives. The kernel drops the lock when the process exits, so a reader tells a live record from a dead one without asking the process. A forked child does not keep its parent's record alive.

Every user can write `/dev/shm`, so a reader believes a record only as far as the kernel vouches for it:

| Record | Believed when |
| --- | --- |
| A bind record, in `CYRA_RUN_DIR` | It is a regular file (not a link, not a fifo) writable by its owner only, its owner is the user of the process its name gives, that process started at the time its name gives, and it is locked. Its lines about other processes are ignored |
| The daemon's record, in `CYRA_DAEMON_DIR` (default `/run/cyra`) | The same, except that its lines may name any process: the directory vouches for them, so it must be writable by its owner alone and the file must be that owner's |

A record carries its length and a checksum, so one read in the middle of its rewrite is read again rather than half believed.

- `bindings()` lists every stage held on this host: `Binding` has `node`, `group`, `name`, `pid`, and `tid`. Records of exited processes and entries of ended threads are left out. Reading deletes nothing; the next process that creates its record sweeps the dead ones.
- `verify_process(spec, pid)` asks the `verify_process(spec)` question about another process, with the holders read from its record. A process that is gone is an `observed-process` Fail. A process no record names (neither its own nor the daemon's) gets a `record` Unknown, since which of its threads hold a stage is then not known.
- When the record cannot be written, the apply still stands and its report carries a `record` Unknown: other processes will not see that thread.

`ThreadState` has `policy` (the raw `SCHED_*` number, since a thread may hold a class `Policy` does not name), `priority`, `cpus`, and `reset_on_fork`.

## 5. flux `PartitionedExecutor`

`flux::ros::PartitionedExecutor` and `flux2::PartitionedExecutor` give each callback group its own thread. `cyra_flux` has the core functions of the same name with the executor, node, and group in front. The executor is a template parameter, and it is one of these two types. `cyra_flux` links neither, so include the executor's own header.

The functions are in `cyra/flux.hpp`. Each group takes one declaration, so the three forms go to three groups here.

```cpp doc:flux
cyra::flux::bind(ex, *node, grab_group, "grab");  // the stage from CYRA_SPEC
cyra::flux::apply_checked(ex, *node, infer_group, opts, cyra::Strictness::Hard);  // values
cyra::flux::apply_checked(ex, *node, loop_group, spec.stage(name, "loop"));  // a Spec's stage
```

- `group` must belong to `node`. `nullptr` means the node's default group.
- An rclcpp callback group has no name, so the call is the only link between the file's `(node, label)` and a thread. For a stage it is checked at the call: the stage names this node, an empty label goes with the default group, and a non-empty label with any other group. An `external` stage is refused.
- One declaration per group, and `ex` must serve the group. A second declaration throws at the call; a declaration for a group no child serves is refused by `spin()`.
- Call `spin()` from a SCHED_OTHER thread. Every child starts on the calling thread's scheduling, so under an RT caller a group with no declaration runs at that priority. `verify_process` lists such threads.
- Put every input of one message_filters synchronizer in one group. The sync policy runs the matched callback under its own `std::mutex`, which has no priority inheritance, so inputs on two group threads tie the two groups' priorities together.
- The message_filters path allocates (policy queues, a `shared_ptr` control block per frame). Keep it out of a hard chain. Plain flux delivery does not allocate.

## 6. Python

```python doc:python
import cyra
import cyra.flux

cyra.bind(node.get_fully_qualified_name(), "loop")
t = cyra.spawn("/perception_node", "infer", run_inference)
cyra.flux.bind(ex, node, group, "grab")

rep = cyra.apply_checked(cyra.Options(cyra.Policy.Fifo, 20, [4]), cyra.Strictness.Soft)
if not rep.ok():
    print(rep.to_string())
state = cyra.current()          # state.policy compares with os.SCHED_FIFO
```

Every name in sections 1 to 5 exists in `cyra` or `cyra.flux` with the same arguments. `Options(policy=, priority=, cpus=, children=)` takes keywords. Errors map as follows.

| C++ | Python |
| --- | --- |
| `std::system_error` (the kernel refused) | `OSError` |
| `std::invalid_argument` (a request that would be reinterpreted, an external stage, a stage already held) | `ValueError` |
| `std::runtime_error` (preflight blocked, read-back mismatch, a file rule, an unknown label) | `RuntimeError` |

Python is not an RT target. Whatever priority a thread holds, the GIL and GC stay unbounded sources of latency. What cyra settles for Python is which thread the kernel prefers when several are runnable and which cores each may use; that matters in the sections that release the GIL (numpy, decoding, I/O).

## 7. Where C++ and Python differ

| Difference | Why |
| --- | --- |
| `spawn` returns `std::thread` in C++ and a started `threading.Thread` in Python | A Python function has to run on a thread Python started |
| `cyra.flux` does not check that `group` belongs to `node` | A flux Python group does not know its node. The stage's node is still checked against `node` |
| In `cyra.flux`, `group=None` is the node's unit (a node given to `add_ros_node`, or `add_node` in flux2) | rclpy splits the unit into groups and nodes; C++ has only groups |
| `cyra.flux.apply_checked` takes `Options` or a `Stage` in one function | Python has no overloads |

`cyra_py/test/test_surface.py` fails when a public name exists on one side only.

## 8. What cyra does not do

- It does not guarantee timing. Memory bandwidth interference, GPU driver ordering, timer phase, and sensor disparity are not solved by priorities. Passing preflight means the host is configured, not that a deadline holds.
- The library does not set threads it cannot run code on (DDS internals, SDK threads, IRQ threads). The daemon `cyrad` does, for stages with `match`; without it, declare them `external` and `verify` them.
- It does not derive priorities. You write them; the file rules check them.
- An RT stage must block regularly. On PREEMPT_RT the per-cpu timer and RCU threads (`ktimers/N`, `rcuc/N`) run at FIFO 1, so a stage that never sleeps starves them on its cpu, and with `sched_rt_runtime_us=-1` nothing takes the cpu back.

## 9. The connection to flux

`cyra_flux` and `cyra.flux` register the `on_thread_start` hook of a flux or flux2 executor, which they take as a template parameter (C++) or any object (Python), linking neither. flux and flux2 know nothing about cyra, and `cyra_core` depends on neither. The hook, the template, and the package split may change at any time; code outside `cyra_flux` and `cyra.flux` must not rely on them.
