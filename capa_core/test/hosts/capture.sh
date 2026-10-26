#!/usr/bin/env bash
# Prints the host files capa's preflight reads, in the format of test/hosts/*.host: a line
# "=== <path>" and then the file, or "=== <dir>/" for a directory. Reads only.
#
# Left out: environ, maps and auxv (the shell's environment and file paths), processes other than
# RT or unbound kernel threads and a few daemons, and every status line preflight does not read.
# Disk UUIDs on the boot line are blanked. Run as root to include debugfs.
#
# The output still identifies the host: kernel build string, kernel threads, cgroup paths, process
# names. It is a reference for writing a fixture by hand, not a fixture to commit.
set -u

echo "capture.sh: this snapshot identifies the host (kernel build, kernel threads, cgroup paths," \
  "process names); do not commit it as-is, write the fixture by hand from it" >&2

emit() {
  local content
  [ -f "$1" ] || return 0
  content=$(cat "$1" 2>/dev/null) || return 0
  printf '=== %s\n%s\n' "$1" "$content"
}

emit_grep() {
  local content
  content=$(grep -E "$2" "$1" 2>/dev/null) || return 0
  printf '=== %s\n%s\n' "$1" "$content"
}

dir() {
  [ -d "$1" ] && printf '=== %s/\n' "$1"
  return 0
}

printf '=== /proc/cmdline\n%s\n' "$(sed -E 's/UUID=[^ ]*/UUID=0/g' /proc/cmdline)"
for f in /proc/version /proc/consoles /proc/self/limits /proc/self/cgroup /proc/self/attr/current \
  /proc/irq/default_smp_affinity; do
  emit "$f"
done
emit_grep /proc/meminfo '^MemTotal:'
printf '=== /proc/cpuinfo\n%s\n' "$(grep -m1 -E '^flags' /proc/cpuinfo)"
emit_grep /proc/self/status '^(Name|Tgid|Pid|CapEff|Seccomp|Seccomp_filters|VmSize|VmLck|Cpus_allowed_list):'
emit_grep /proc/self/mountinfo ' - proc '
for f in sched_rt_runtime_us sched_rt_period_us watchdog soft_watchdog watchdog_cpumask \
  numa_balancing printk tainted; do
  emit "/proc/sys/kernel/$f"
done
for f in compact_unevictable_allowed dirty_ratio dirty_bytes dirty_background_ratio \
  dirty_background_bytes; do
  emit "/proc/sys/vm/$f"
done

for d in /proc/irq/[0-9]*; do
  emit "$d/effective_affinity_list"
  emit "$d/smp_affinity_list"
  for h in "$d"/*/; do
    [ -d "$h" ] && dir "${h%/}"
  done
  emit "/sys/kernel/debug/irq/irqs/${d##*/}"
done

for p in /proc/[0-9]*; do
  stat=$(cat "$p/stat" 2>/dev/null) || continue
  rest=${stat##*) }
  read -r -a f <<< "$rest"
  comm=$(cat "$p/comm" 2>/dev/null)
  kernel=$(( f[6] & 0x200000 ))
  allowed=$(grep -E '^Cpus_allowed_list:' "$p/status" 2>/dev/null)
  case "$comm" in
    irqbalance|power-profiles-|tuned|thermald|systemd-oomd|auditd) keep=1 ;;
    *) keep=0 ;;
  esac
  if [ "$kernel" -ne 0 ] && { [ "${f[38]}" -ne 0 ] || [[ "$allowed" =~ [-,] ]]; }; then
    keep=1
  fi
  [ "$keep" -eq 1 ] || continue
  printf '=== %s/task/%s/stat\n%s\n' "$p" "${p##*/}" "$stat"
  printf '=== %s/task/%s/status\n%s\n' "$p" "${p##*/}" \
    "$(grep -E '^(Name|Tgid|Pid|VmSize|Cpus_allowed_list):' "$p/status")"
done

cpu=/sys/devices/system/cpu
for f in online isolated nohz_full intel_pstate/no_turbo intel_pstate/status cpufreq/boost; do
  emit "$cpu/$f"
done
for c in "$cpu"/cpu[0-9]*; do
  for f in scaling_governor scaling_driver energy_performance_preference scaling_min_freq \
    cpuinfo_max_freq related_cpus cpuinfo_transition_latency; do
    emit "$c/cpufreq/$f"
  done
  for s in "$c"/cpuidle/state*; do
    for f in name latency disable; do emit "$s/$f"; done
  done
  for f in power/pm_qos_resume_latency_us topology/thread_siblings_list cpu_capacity \
    cache/index3/shared_cpu_list; do
    emit "$c/$f"
  done
  emit "/sys/kernel/debug/sched/fair_server/${c##*/}/runtime"
  emit "/sys/kernel/debug/sched/fair_server/${c##*/}/period"
done

for f in /sys/devices/system/node/online \
  /sys/devices/system/clocksource/clocksource0/current_clocksource \
  /sys/devices/virtual/workqueue/cpumask /sys/fs/cgroup/cpuset.cpus.isolated \
  /sys/kernel/realtime /sys/kernel/sched_ext/state /sys/kernel/sched_ext/root/ops \
  /sys/kernel/mm/transparent_hugepage/enabled /sys/kernel/mm/transparent_hugepage/defrag \
  /sys/kernel/mm/transparent_hugepage/khugepaged/defrag /sys/kernel/debug/sched/preempt \
  /sys/hypervisor/type /sys/firmware/acpi/platform_profile; do
  emit "$f"
done
dir /proc/device-tree/hypervisor
[ -e /etc/nvpmodel.conf ] && printf '=== /etc/nvpmodel.conf\n\n'

group=/sys/fs/cgroup$(sed -n 's/^0:://p' /proc/self/cgroup)
group=${group%/}
while :; do
  for f in cpuset.cpus.effective cpuset.cpus.partition cpu.max memory.high memory.max \
    memory.swap.max; do
    emit "$group/$f"
  done
  [ "$group" = /sys/fs/cgroup ] && break
  group=${group%/*}
done
