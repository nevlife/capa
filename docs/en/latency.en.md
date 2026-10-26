# Measuring latency

cyra sets and checks scheduling; it does not measure latency. This is the order to measure a host in, with the kernel's `rtla` and the `rt-tests` suite. Run `cyra check` first: its Warns are the configuration the measurement will show.

## Layers

Measure from the bottom up. A layer is never better than the one below it.

| Layer | Tool | Shows |
| --- | --- | --- |
| Hardware | `rtla hwnoise` | SMIs and firmware stalls that nothing in Linux sees |
| Timer interrupt | `rtla timerlat`, IRQ column | From the timer firing to its handler |
| Timer thread | `rtla timerlat`, thread column | From the timer firing to the woken thread running |
| User space | `cyclictest` | The same from a user process, through the syscall path |
| CPU noise | `rtla osnoise` | Everything else that runs on the cpu |

## Conditions

- Load the host the way it will run: CPU, I/O, network and memory load (`stress-ng`), or `rteval`. Start after the temperature settles.
- Plan for 70 to 80% of the deadline, and judge by p99 and max.
- Max grows with the run's length, so compare runs of equal length only: 5 to 10 minutes for a check, 30 to 60 for a regression, 24 hours for rare events.
- Keep the interrupts of the NIC your SSH session uses off the measured cpus.
- Do not interpret results from a virtual machine (`virtualization`).

## timerlat

`rtla timerlat` runs its thread at FIFO 95 by default, above most stages, so the number it reports is lower than a stage would see. Give it the stage's priority, and take the SCHED_OTHER baseline separately.

```text
rtla timerlat hist -c 3 -P f:80 -d 10m -b 1 -E 1000
rtla timerlat hist -c 3 -P o:0 -d 10m -b 1 -E 1000
rtla timerlat top -c 3 -P f:80 -a 100
```

- In CI always pass `-d`, so the run ends.
- Size the histogram with `-b` (bucket width) and `-E` (buckets) so that no sample lands in `over`.
- `-a <us>` stops at the first sample over the bound and prints what ran instead.

## osnoise

```text
rtla osnoise top -c 3 -d 10m
```

An isolated cpu shows 99.99% or more of `%CPU` available. When osnoise is clean but the timerlat thread column is high, a higher RT thread or a DEADLINE task (the fair server included) takes the cpu: see `foreign-rt-thread`, `deadline-task` and `rt-throttle`.

## cyclictest

```text
cyclictest --mlockall --smp --priority=80 --interval=200 --distance=0 -D 60
trace-cmd record -e irq -e sched -e ipi -- cyclictest --mlockall --smp --priority=80 --interval=200 --distance=0 -D 60
```

When `mpstat -P ALL` shows a cpu over 30% `%irq` or 20% `%soft`, find the source before measuring further. `verify` with a window names the rows (`cpu-interrupts`, `cpu-softirqs`).

`pip_stress` (rt-tests) checks that PI futexes work on this kernel.

## Kernels without rtla

Jetson's 5.15 has the osnoise and timerlat tracers but not the `rtla` tool. Use tracefs at `/sys/kernel/tracing` directly, after checking which tracers the kernel has.

```text
cd /sys/kernel/tracing
cat available_tracers               # osnoise, timerlat, irqsoff, preemptoff, wakeup_rt
echo 3 > osnoise/cpus
echo 1000 > osnoise/timerlat_period_us
echo timerlat > current_tracer
cat trace_pipe
echo 5 > tracing_thresh             # osnoise: the smallest gap counted as noise, in us
echo osnoise > current_tracer
```

## Runtime verification

From 6.17 the `rtapp` runtime verification monitor, with its `pagefault` and `sleep` monitors, reports in the kernel when an RT task takes a page fault or sleeps where it should not. Enable it only while testing.
