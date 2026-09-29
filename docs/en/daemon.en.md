# The daemon `cyrad`

The library sets a thread from inside it: `bind`, `spawn`, and the flux adapter run on the thread they set. That leaves the threads no code of ours runs on: interrupt threads, DDS and SDK threads. `cyrad` (package `cyra_daemon`) sets those from outside, for the stages that carry `match`. Everything else works without it.

```text
cyrad [--spec FILE] [--interval SEC] [--once]
```

The file is `--spec`, else `CYRA_SPEC`. Every `--interval` seconds (default 1) it makes one pass. `--once` makes one pass and exits, leaving what it set in place.

| Signal | Effect |
| --- | --- |
| SIGHUP | Reread the file. A file that breaks a rule is refused and the old one kept. Threads of stages that still match the same way stay set; the rest are put back. Children it moved follow their stage's new `children_cpus`, and go back where they ran when their stage no longer has any |
| SIGTERM, SIGINT | Put every thread it set, and every child it moved, back to what it ran at before, and exit |

What it set and moved, with what each thread ran at before, cyrad keeps in `cyrad.state` in `CYRA_DAEMON_DIR` (default `/run/cyra`), rewritten whole whenever it changes. A cyrad killed without its stop (`kill -9`, a crash) leaves the file behind, and the next cyrad takes over from it before its first pass, as a reload does: threads of stages still matched the same way stay its own with their first values, the rest go back at once. A stop after a restart therefore puts threads back where they started. `/run` is emptied at boot, and so are the threads.

## One pass

| Stage | What cyrad does |
| --- | --- |
| `match` | Finds the threads the match names. Exactly one: runs `preflight` for it, with memory judged on that thread's process, and skips it when that refuses (a `Fail`, or a `Warn` on a hard chain). Otherwise sets it, read back like `apply_checked`, and puts it back when it drifts. None, two or more, a refusal, or a different thread than before: the thread it set before goes back to what it ran at first, and nothing is set |
| bound by a thread (`bind`, `spawn`, flux) | Reports drift and changes nothing: the thread owns its stage. With `children_cpus`, moves the stage's reset children there |
| `external` | Nothing |

- cyrad sets only a thread it can put back: one running SCHED_OTHER, SCHED_FIFO or SCHED_RR. A thread on SCHED_BATCH, SCHED_IDLE or SCHED_DEADLINE is reported and left alone.
- A thread is told from a later thread reusing its tid by its start time. When that changes while cyrad sets or moves it, cyrad says so and does not keep it.

A reset child is recognized by what it inherited, since the kernel does not say which thread started which: a thread of the holder's process that holds no stage, was not there yet when the stage was bound (the bind records the threads it found), runs SCHED_OTHER, and is allowed exactly the stage's cpus. A thread started after the bind by another thread that pins itself to exactly those cpus looks the same and is moved too. What cyrad moved it remembers, with where it ran before.

cyrad records the stages it set, so `cyra status` shows them with the pid and tid of the thread it set. Its record names threads of other processes, so it is kept apart from the bind records, in `CYRA_DAEMON_DIR` (default `/run/cyra`). cyrad creates the directory if missing and refuses to write into one that is not its user's or that others can write; readers ignore such a directory too. cyrad prints one line per change on stderr.

## Matching

| `match` | Finds |
| --- | --- |
| `{irq: 3550000.usb}` | The interrupt thread of that handler: for each `/sys/kernel/irq/<n>/actions` naming it, the thread `irq/<n>-3550000.usb` (cut to 15 bytes on kernels before 5.17). `irq/<n>-s-...` secondary threads are not it |
| `{process: cam, thread: dds.recv}` | The thread with comm `dds.recv` in a process with comm `cam`. comm is 15 bytes, so longer names compare by their first 15 |

A match is exact, not a pattern. Name a thread with `pthread_setname_np` when the library allows it.

## Building

cyrad is Rust. The chain file, its rules and every verdict stay in `cyra_core`; cyrad reaches them through a `cxx` bridge (`src/ffi.rs`, `src/bridge.cpp`), so there is still one parser. colcon builds it like any package: CMake finds `cyra_core`, hands its libraries to cargo, and installs `cyrad`. `colcon test` runs `cargo test`.

- It needs rustup's toolchain. `rust-toolchain.toml` pins 1.98.1. A distribution cargo (apt's 1.75 on Ubuntu 24.04) ignores that file and cannot build the 2024 edition. CMake looks in `$CARGO_HOME/bin` (default `~/.cargo/bin`) before `PATH`.

  ```text
  curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- --default-toolchain 1.98.1
  ```

- The first build downloads the crates `Cargo.lock` pins, so it needs the network once.

## Permission

Setting another user's thread needs `CAP_SYS_NICE`. `systemd/cyrad.service` runs it as its own user with only that capability. Without it, a set fails with "setting a thread of another user needs CAP_SYS_NICE" and cyrad reports it and goes on.

A matched thread runs unscheduled until cyrad finds it. Interrupt threads exist from boot, so starting cyrad before the application closes that window for them. A thread you can run code on should `bind` itself instead: it is then set before it does any work.
