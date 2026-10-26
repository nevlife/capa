# The daemon `capad`

The library sets a thread from inside it: `bind`, `spawn`, and the flux adapter run on the thread they set. That leaves the threads no code of ours runs on: interrupt threads, DDS and SDK threads. `capad` (package `capa_daemon`) sets those from outside, for the stages that carry `match`. Everything else works without it.

```text
capad [--spec FILE] [--interval SEC] [--once]
```

The file is `--spec`, else `CAPA_SPEC`. Every `--interval` seconds (default 1) it makes one pass. `--once` makes one pass and exits, leaving what it set in place.

| Signal | Effect |
| --- | --- |
| SIGHUP | Reread the file. A file that breaks a rule is refused and the old one kept. Threads of stages that still match the same way stay set; the rest are put back. Children it moved follow their stage's new `children_cpus`, and go back where they ran when their stage no longer has any |
| SIGTERM, SIGINT | Put every thread it set, and every child it moved, back to what it ran at before, and exit |

What it set and moved, with what each thread ran at before, capad keeps in `capad.state` in `CAPA_DAEMON_DIR` (default `/run/capa`), rewritten whole whenever it changes. A capad killed without its stop (`kill -9`, a crash) leaves the file behind, and the next capad takes over from it before its first pass, as a reload does: threads of stages still matched the same way stay its own with their first values, the rest go back at once. A stop after a restart therefore puts threads back where they started. `/run` is emptied at boot, and so are the threads.

## One pass

| Stage | What capad does |
| --- | --- |
| `match` | Finds the threads the match names. Exactly one: runs `preflight` for it, with memory judged on that thread's process, and skips it when that refuses (a `Fail`, or a `Warn` on a hard chain). Otherwise sets it, read back like `apply_checked`, and puts it back when it drifts. None, two or more, or a refusal: the thread it set before goes back to what it ran at first, and nothing is set. A different thread than before: the old one goes back first, then the new one is set |
| bound by a thread (`bind`, `spawn`, flux) | Reports drift and changes nothing: the thread owns its stage. With `children_cpus`, moves the stage's reset children there |
| `external` | Nothing |

- capad sets only a thread it can put back: one running SCHED_OTHER, SCHED_FIFO or SCHED_RR. A thread on SCHED_BATCH, SCHED_IDLE or SCHED_DEADLINE is reported and left alone.
- A thread is told from a later thread reusing its tid by its start time. When that changes while capad sets or moves it, capad says so and does not keep it.

A reset child is recognized by what it inherited, since the kernel does not say which thread started which: a thread of the holder's process that holds no stage, was not there yet when the stage was bound (the bind records the threads it found), runs SCHED_OTHER, and is allowed exactly the stage's cpus. A thread started after the bind by another thread that pins itself to exactly those cpus looks the same and is moved too. What capad moved it remembers, with where it ran before.

capad records the stages it set, so `capa status` shows them with the pid and tid of the thread it set. Its record names threads of other processes, so it is kept apart from the bind records, in `CAPA_DAEMON_DIR` (default `/run/capa`). capad creates the directory if missing and refuses to write into one that is not its user's or that others can write; readers ignore such a directory too. capad prints one line per change on stderr.

## Matching

| `match` | Finds |
| --- | --- |
| `{irq: 3550000.usb}` | The interrupt thread of that handler: for each `/sys/kernel/irq/<n>/actions` naming it, the thread `irq/<n>-3550000.usb` (cut to 15 bytes on kernels before 5.17). `irq/<n>-s-...` secondary threads are not it |
| `{process: cam, thread: dds.recv}` | The thread with comm `dds.recv` in a process with comm `cam`. comm is 15 bytes, so longer names compare by their first 15 |

A match is exact, not a pattern. Name a thread with `pthread_setname_np` when the library allows it.

## Building

capad is Rust. The chain file, its rules and every verdict stay in `capa_core`; capad reaches them through a `cxx` bridge (`src/ffi.rs`, `src/bridge.cpp`), so there is still one parser. colcon builds it like any package: CMake finds `capa_core`, hands its libraries to cargo, and installs `capad`. `colcon test` runs `cargo test`.

- It needs rustup's toolchain. `rust-toolchain.toml` pins 1.98.1. A distribution cargo (apt's 1.75 on Ubuntu 24.04) ignores that file and cannot build the 2024 edition. CMake looks in `$CARGO_HOME/bin` (default `~/.cargo/bin`) before `PATH`.

  ```text
  curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- --default-toolchain 1.98.1
  ```

- The first build downloads the crates `Cargo.lock` pins, so it needs the network once.

## Permission

Setting another user's thread needs `CAP_SYS_NICE`. `systemd/capad.service` runs it as its own user with only that capability. Without it, a set fails with "setting a thread of another user needs CAP_SYS_NICE" and capad reports it and goes on.

A matched thread runs unscheduled until capad finds it. Interrupt threads exist from boot, so starting capad before the application closes that window for them. A thread you can run code on should `bind` itself instead: it is then set before it does any work.
