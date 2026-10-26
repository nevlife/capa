# capa 사용자 API

capa는 데이터 경로 위 스레드의 OS 스케줄링(정책·우선순위·CPU, 자식에게 물려줄 것)을 걸고 확인한다. 스케줄은 파일 하나에 두고, 코드는 스레드가 어느 stage인지만 말한다.

| 패키지 | 내용 | 의존 |
| --- | --- | --- |
| `capa_core` | C++17, ROS 미사용. 체인 파일, 적용, 호스트 preflight, 되읽기, 확인 | yaml-cpp |
| `capa_flux` | flux `PartitionedExecutor` 스레드용으로 같은 함수 | `capa_core`, `rclcpp` |
| `capa_py` | Python. `capa`(`capa_core`의 이름)와 `capa.flux`(`capa_flux`의 이름) | `capa_core` |

C++와 Python은 이름, 인자 순서, 기본값이 같다. 다른 곳은 언어가 강제하는 곳뿐이고 7절에 모았다.

## 1. 스레드마다 한 줄

파일이 체인의 모든 stage와 그 스케줄을 적는다. `CAPA_SPEC`이 그 파일을 가리킨다.

```yaml
chains:
  perception_to_control:
    target: hard
    stages:
      - {node: /camera_node, group: grab, policy: fifo, priority: 60, cpus: [2]}
      - {node: /perception_node, group: infer, policy: fifo, priority: 70, cpus: [3]}
      - {node: /control_node, group: loop, policy: fifo, priority: 80, cpus: [4]}
```

스레드는 자기가 어느 stage인지만 말한다. 어떤 호출을 쓰는지는 스레드를 누가 만들었는지로 정해진다.

| 스레드 | C++ | Python |
| --- | --- | --- |
| 새로 만드는 스레드 | `capa::spawn("/perception_node", "infer", fn)` | `capa.spawn("/perception_node", "infer", fn)` |
| 이미 도는 스레드(main, 직접 가진 스레드) | `capa::bind("/control_node", "loop")` | `capa.bind("/control_node", "loop")` |
| flux `PartitionedExecutor` 그룹 스레드 | `capa::flux::bind(ex, *node, group, "grab")` | `capa.flux.bind(ex, node, group, "grab")` |

- `bind(target, group="")`는 호출한 스레드를 그 stage에 걸고, 호스트를 판정하고, 커널에서 되읽는다. `target`은 노드의 전체 이름(`get_fully_qualified_name()`)과 그룹 라벨, 또는 `name` stage의 이름(그룹 없이)이다.
- `spawn(target, group, fn)`은 스레드를 만들어 bind하고 그 뒤에 `fn`을 돌린다. 스레드가 걸린 뒤에 반환한다. bind가 거절되면 `fn`은 돌지 않고 거절이 호출자에게 던져진다. 스레드 안에서 `bind`를 부르는 방식보다 이것을 쓴다. 그 방식은 `bind` 줄에 닿을 때까지 스케줄 없이 돈다.
- `capa::flux::bind(ex, node, group, label="")`는 executor에 bind를 등록한다. `group`을 맡은 자식이 첫 콜백 전에 자기에게 걸고, 거절은 `spin()`의 에러가 된다. stage는 호출 시점에 찾으므로 오타는 그 자리에서 예외를 던진다.
- `CAPA_SPEC`이 없으면 셋 다 아무것도 하지 않는다. `bind`는 빈 report를 돌려주고, `spawn`은 `fn`만 돌리고, `flux::bind`는 아무것도 등록하지 않는다.
- 파일에 없는 라벨은 예외를 던진다. 아무것도 안 걸고 넘어가면 오타가 숨는다.

## 2. 체인 파일

```yaml
chains:
  perception_to_control:
    target: hard                  # hard: preflight Warn이 적용을 막는다. soft: 보고만 한다
    criticality: 2                # 높을수록 공유 CPU에서 먼저. 기본 0
    stages:
      - name: camera_usb_irq      # ROS 스레드가 아니면 name으로 부른다
        kind: release             # 일을 넘겨주기만 한다. 먹이는 worker보다 위에서 돈다
        external: true            # capa가 걸지 않는다. 기본값 50에서 chrt로 올렸다
        expect_priority: 85       # external은 어느 CPU에나 올 수 있으므로 모든 worker보다 위
      - node: /camera_node
        group: grab               # 생략 = 노드의 기본 콜백 그룹
        policy: fifo              # inherit / other / fifo / rr
        priority: 55
        cpus: [2]
        children: reset           # reset(기본) / inherit
      - node: /perception_node
        group: infer
        policy: fifo
        priority: 70
        cpus: [3]
        children: inherit         # OpenMP 팀 스레드가 이 stage의 일을 한다
      - node: /control_node
        group: loop
        policy: fifo
        priority: 80
        cpus: [4]
```

| 키 | 위치 | 뜻 |
| --- | --- | --- |
| `target` | 체인 | `hard` 또는 `soft`. 필수 |
| `criticality` | 체인 | 0 이상, 기본 0 |
| `stages` | 체인 | 데이터가 흐르는 순서의 스레드들 |
| `node`, `group` | stage | ROS 스레드. 노드 전체 이름과 그룹 라벨 |
| `name` | stage | 그 밖의 스레드. `/`로 시작하지 않고 `group`을 갖지 않는다 |
| `kind` | stage | `worker`(기본)는 계산한다. `release`는 일을 넘겨주기만 한다. IRQ 스레드, DDS 수신 스레드, 타이머 스레드 |
| `policy`, `priority`, `cpus`, `children` | stage | capa가 거는 것(3절) |
| `external`, `expect_priority` | stage | capa가 걸지 않는 스레드. `expect_priority`(1..99, 0은 미선언)만 적는다. 거는 진입점은 전부 거절하고 `verify`가 확인한다 |
| `match` | stage | `{irq: <handler>}` 또는 `{process: <comm>, thread: <comm>}`. 우리 코드를 넣을 수 없는 스레드로, 데몬 `capad`가 찾아서 건다. `bind`와 `apply_checked`는 거절한다(`docs/ko/daemon.ko.md`) |
| `children_cpus` | stage | `capad`가 이 stage의 reset 자식을 옮길 곳. `cpus`, `children: reset`이 있어야 하고 `cpus`와 겹치지 않아야 한다 |

규칙은 파일을 읽을 때 검사한다. 어긴 stage를 이름으로 보고한다.

| 규칙 | 검사 |
| --- | --- |
| 흐름 | 한 체인에서 CPU를 공유할 수 있는 두 worker는 하류가 상류와 같거나 높다. 같아도 된다. FIFO는 같은 우선순위를 선점하지 않는다. CPU가 겹치지 않는 stage는 서로 선점하지 않으므로 비교하지 않는다 |
| release | release stage는 CPU를 공유할 수 있는 같은 체인의 모든 worker보다 엄격히 높다. 아래에 있으면 넘겨줄 일이 늦게 풀린다 |
| 중요도 | 공유 CPU에서 더 중요한 체인의 RT stage는 덜 중요한 체인의 RT stage보다 전부 엄격히 높다. 여러 체인에 속한 stage는 그중 가장 높은 중요도를 가진다 |
| hard 코어 | hard 체인이 걸린 곳에서 RT worker 둘을 한 CPU에 핀하지 않는다. SCHED_FIFO에는 타임슬라이스가 없어 하나가 block할 때까지 코어를 쥔다. release stage는 세지 않는다 |
| 선언 하나 | 두 체인에 나오는 stage는 두 곳에서 같게 선언한다. strictness는 그중 가장 엄격한 것이다 |
| 이름 하나 | 두 stage가 같은 이름이 되지 않는다. 노드 `/foo/bar`의 기본 그룹과 노드 `/foo`의 그룹 `bar`는 둘 다 `/foo/bar`다. 메시지, 기록, 데몬은 stage를 이 이름으로 부른다 |

- "CPU를 공유할 수 있다"는 `cpus`가 겹친다는 뜻이다. `cpus`가 없는 stage와 external stage는 어디에나 올 수 있으므로 모든 CPU를 공유한다.
- RT가 아닌 stage(`policy: other`, `inherit`, `expect_priority: 0`인 external)는 어떤 규칙에도 들어가지 않는다.
- 모르는 키는 거절한다. 버전 필드는 없다. 버전 번호는 스키마 교체를 알릴 뿐 `priorty` 같은 오타를 못 잡는다.
- `capa::Spec::load(path)`는 파일을, `capa::Spec::load()`는 `CAPA_SPEC`을 읽는다. 변수가 없으면 빈 spec이고 `empty()`가 참이다. `stages()`는 전부, `find(target, group)`는 하나 또는 null, `stage(target, group)`는 하나 또는 예외, `external()`은 external만 준다.
- `capa::Stage`는 `match`(`irq`, `process`, `thread`, `empty()`를 가진 `Match`), `children_cpus`, `node`, `group`, `name`, `kind`(`Kind::Worker` 또는 `Kind::Release`), `external`, `expect_priority`, `opts`, `strict`, `chain`(처음 선언한 체인)을 담는다.

## 3. 적용

여기 있는 것은 전부 호출한 스레드 자신에게만 건다. 밖에서 스레드를 걸지 않으므로, 다른 스레드가 절반만 설정한 구간이 생기지 않는다.

함수는 `capa/spec.hpp`에 있고, 이 헤더가 `capa/rt.hpp`를 포함한다.

```cpp doc:apply
capa::Options o;
o.policy = capa::Policy::Fifo;       // Inherit / Other / Fifo / RoundRobin
o.priority = 70;                     // Fifo/RoundRobin이면 1..99, 아니면 0
o.cpus = {3};                        // 비우면 affinity를 안 건드린다
o.children = capa::Children::Reset;  // Reset / Inherit

capa::Report rep = capa::apply_checked(o, capa::Strictness::Hard, /*control_priority=*/80);
rep = capa::apply_checked(spec.stage("/perception_node", "infer"));  // Spec의 stage
rep = capa::bind("/perception_node", "infer");                       // CAPA_SPEC의 stage
```

- `apply_checked(opts, strict=Soft, control_priority=0)`는 `preflight`를 먼저 돌린다. 판정이 막으면 스레드를 건드리기 전에 `std::runtime_error`를 던진다. 그다음 걸고 커널에서 되읽는다. policy와 priority는 같아야 하고, 스레드의 CPU는 요청한 CPU 안에 있어야 한다(cpuset이 마스크를 좁힐 수 있다). 어긋나면 예외를 던진다. 이때 스레드는 이미 바뀐 상태다.
- `Strictness`가 `Warn`을 읽는 방법을 정한다. `Soft`는 report로 돌려주고 `Hard`는 거절한다. `Fail`은 둘 다 거절한다.
- `apply_checked(stage)`와 `bind`는 stage를 통째로 받아 체인의 strictness가 함께 온다. report의 `priority-order`는 Ok다. 순서는 파일을 읽을 때 이미 검사됐다.
- 한 stage는 한 스레드다. 이 프로세스의 살아 있는 스레드 하나가 stage를 쥐고 있는 동안 다른 스레드가 걸면 `std::invalid_argument`를 던진다. 쥔 스레드가 다시 거는 것은 허용한다. 쥔 스레드가 끝나면 풀린다.
- 아무것도 요청하지 않는 `Options`는 판정까지 포함해 아무것도 하지 않고 빈 report를 돌려준다.
- `Options::validate()`는 다르게 해석될 요청을 거절한다. 대상은 priority 1..99가 없는 RT 정책, RT 정책 없는 priority, RT 정책 없는 `children = Inherit`, 커널 집합 크기를 넘는 CPU 번호다.
- 커널의 거절은 `std::system_error`다. 조용한 다운그레이드는 없다.

### 자식

Linux는 새 스레드에 부모의 정책, 우선순위, CPU를 준다. `fork()`와 `exec()`도 그대로 유지한다. RT 스레드가 라이브러리를 부르면, 그 라이브러리가 만드는 풀, 로거, `system()`에 RT가 전부 샌다.

| `children` | 자식의 시작 상태 | 쓰는 곳 |
| --- | --- | --- |
| `Reset`(기본) | SCHED_OTHER, 우선순위 0 | 라이브러리 풀, 로거, 하위 프로세스. 이 stage로 모델링하지 않은 모든 것 |
| `Inherit` | 부모의 정책과 우선순위 | 이 stage의 일을 하는 fork-join 협력 스레드. OpenMP 팀 스레드 등 |

- `Reset`은 `SCHED_RESET_ON_FORK`다. `fork`뿐 아니라 `pthread_create`에도 먹고, RT 정책일 때만 효과가 있다.
- affinity는 어느 쪽이든 물려받는다. 커널에 막는 플래그가 없다. 그래서 reset 자식도 stage의 CPU에서 돈다. stage 스레드가 잠들 때만 돈다.
- `Reset`으로 건 스레드는 `CAP_SYS_NICE` 없이는 플래그를 끌 수 없다. 같은 스레드를 나중에 `Inherit`으로 바꾸려면 그 권한이 필요하고, 거절 메시지가 이 이유를 말한다.
- 자기 스케줄이 필요한 오래 사는 helper는 따로 stage로 선언한다. helper 안에서 bind하거나 `external`로 선언한다.

## 4. 확인

```cpp doc:check
capa::Report host = capa::preflight(o, /*control_priority=*/80);  // 판정만, 변경 없음
capa::ThreadState self = capa::current();                          // 이 스레드, 커널에서
int tid = capa::this_tid();                                        // 이 스레드의 커널 id
capa::ThreadState other = capa::observe(tid);                      // 아무 스레드, 프로세스 무관
capa::Report drift = capa::verify(stage, tid);                     // 스레드와 stage 대조
capa::Report stray = capa::verify_process(spec);                   // 설명되지 않는 RT 스레드
capa::Report whole = capa::preflight(spec);                        // 파일 전체를 이 호스트에서
capa::Report spent = capa::verify(stage, tid, std::chrono::seconds(1));  // 카운터까지
```

`Report`는 `findings`를 담는다. `Finding`은 `id`(안정 슬러그), `verdict`, `detail`이다. `Verdict`는 `Ok`, `Warn`, `Fail`, `Unknown`, `Info`다. `Unknown`은 소스를 읽지 못한 것이지 통과가 아니다. `Info`는 알아 둘 사실이고, 어떤 strictness에서도 막지 않는다. `ok()`는 "`Fail` 없음"이다. `find(id)`는 finding 하나나 null을 준다. `to_string()`은 사람이 읽는 것이고 파싱할 형식이 아니다.

`preflight(opts, control_priority=0, strict=Soft)`는 요청 하나에 대해 호스트를 판정한다. `strict`는 그 요청을 어떻게 읽을지다. 아래 finding 몇 개는 hard stage면 Warn, soft stage면 Info다. `apply_checked`와 `bind`는 자기 strictness를 넘긴다. 적용을 막을 수 있는 것은 Warn과 Fail뿐이고 Info는 막지 않는다.

`preflight` finding. 어느 것이 도는지는 요청에 달렸다. 첫 묶음은 RT 정책이 있을 때, 둘째 묶음은 `cpus`가 있을 때 돌고, 셋째 묶음은 늘 돈다. 마지막 묶음은 스레드가 속한 프로세스에 관한 것이라 커널 스레드에서는 빠진다. 읽을 수 없는 소스(root 전용 debugfs, lockdown, 다른 사용자의 프로세스)는 Unknown이고 Fail이 되지 않는다.

| id | 소스 | Fail / Warn / Info |
| --- | --- | --- |
| `policy-permission` | `CapEff`(CAP_SYS_NICE)와 `/proc/self/limits`의 `RLIMIT_RTPRIO` | Fail: 권한이 없다. seccomp, LSM 라벨, `hidepid`가 걸려 있으면 detail에 적는다 |
| `rt-throttle` | `/proc/sys/kernel/sched_rt_runtime_us`, debugfs `sched/fair_server/cpuN`(6.12+) | Warn: throttle이나 fair deadline server가 바쁜 RT 스레드의 CPU를 우선순위와 무관하게 뺏는다. 6.12+에서 `-1`인데 fair server를 읽지 못하면 Unknown |
| `priority-order` | `opts.priority`와 `control_priority` | Fail: control loop 이상. `control_priority`가 0이면 Unknown |
| `priority-band` | `opts.priority` | Info: 99. 커널의 stopper와 watchdog 작업이 도는 자리다(10절) |
| `cpu-online` | `/sys/devices/system/cpu/online` | Fail: 없는 CPU |
| `cpu-isolation` | `/sys/devices/system/cpu/isolated`, 루트 `cpuset.cpus.isolated`, `nohz_full` | Warn: 다른 태스크와 CPU를 나눠 쓴다 |
| `cpuset` | 프로세스 cgroup의 `cpuset.cpus.effective`, 루트까지의 `cpuset.cpus.partition` | Fail: cpuset 밖의 CPU(컨테이너, systemd `AllowedCPUs`). Warn: invalid partition. cgroup v2가 없으면(JetPack 5) Unknown |
| `cpu-governor` | `cpufreq/scaling_governor` | Warn: 주파수 상승이 wakeup 지연을 늘린다 |
| `cpufreq` | `scaling_driver`, `energy_performance_preference`, `scaling_min_freq`, `related_cpus`, `cpuinfo_transition_latency`, boost | Warn: EPP가 `performance`가 아니다. active 모드 intel_pstate와 amd-pstate에서는 governor가 아니라 EPP가 정한다 |
| `rcu-offload` | `/proc/cmdline`의 `rcu_nocbs`, `nohz_full` | Warn: 미뤄둔 커널 해제가 RT CPU에서 돈다 |
| `irq-affinity` | `/proc/irq/*/effective_affinity_list` | Warn: 인터럽트 핸들러가 그 CPU의 모든 태스크를 선점한다 |
| `irq-managed` | debugfs `irq/irqs/N` | Warn: managed 인터럽트가 그 CPU로 오고 옮길 수 없다(`isolcpus=managed_irq`) |
| `irq-default-affinity` | `/proc/irq/default_smp_affinity` | Warn: 나중에 생기는 인터럽트(핫플러그 카메라)가 그 CPU에서 시작한다 |
| `irqbalance` | 돌고 있는 `irqbalance` | Warn: 약 10초마다 인터럽트 affinity를 다시 쓴다 |
| `cpu-idle` | `cpuidle/stateN/{latency,disable}`, `power/pm_qos_resume_latency_us` | Warn: 허용된 idle state의 exit latency가 50 us를 넘는다 |
| `workqueue` | `/sys/devices/virtual/workqueue/cpumask` | hard stage면 Warn, soft stage면 Info: unbound 커널 작업이 그 CPU에서 돈다 |
| `kthread-affinity` | CPU 둘 이상에 허용된 커널 스레드 | hard stage면 Warn, soft stage면 Info: 그 CPU에서 돌 수 있는 것이 있다 |
| `smt` | `topology/thread_siblings_list` | hard stage면 Warn, soft stage면 Info: SMT sibling이 격리되지 않았다 |
| `cpu-capacity` | `cpu_capacity` | Info: capacity가 낮은 코어(효율 코어) |
| `softlockup-watchdog` | `kernel.watchdog`, `kernel.watchdog_cpumask` | hard stage면 Warn, soft stage면 Info: 4초 softlockup 타이머가 그 CPU에서 돈다 |
| `kernel-preemption` | `/sys/kernel/realtime`, `/proc/version` | Warn: PREEMPT_RT가 아니다 |
| `preempt-mode` | `/proc/version`, debugfs `sched/preempt` | Warn: `none`이나 `voluntary`, 그리고 RT가 아닌 stage의 `lazy`. debugfs를 읽지 못하면 Unknown |
| `sched-ext` | `/sys/kernel/sched_ext/state` | Warn: BPF 스케줄러가 이 stage나 그 reset 자식을 맡는다 |
| `thp` | `transparent_hugepage/{enabled,defrag}` | Warn: defrag가 `always`라 fault 경로에서 compaction이 돈다. Info: enabled가 `always` |
| `compaction` | `vm.compact_unevictable_allowed` | Info: 1이라 compaction이 mlock된 페이지를 옮긴다 |
| `numa-balancing` | `node/online`, `kernel.numa_balancing` | Info: 노드가 둘 이상인데 켜져 있다 |
| `dirty-limits` | `vm.dirty_bytes`, `vm.dirty_ratio`, `MemTotal` | Info: 비율 한도다. 한도를 넘겨 쓰는 스레드는 잠든다 |
| `audit` | 부트 라인의 `audit=`, `auditd` | Info: audit가 켜져 있다. 둘 다 말이 없으면 Unknown |
| `console` | `/proc/consoles`, `kernel.printk` | Info: PREEMPT_RT가 아닌 커널에 시리얼 콘솔이 있다 |
| `clocksource` | `current_clocksource` | Warn: `hpet`이나 `acpi_pm`이라 읽기 한 번에 1 us쯤 든다. Info: `tsc`, `arch_sys_counter`, `kvm-clock`가 아닌 다른 clocksource |
| `virtualization` | CPU의 `hypervisor` 플래그, `/sys/hypervisor` | Info: 가상 머신이다. 여기서 잰 지연은 실기와 무관하다 |
| `power-policy` | `power-profiles-daemon`, `tuned`, `thermald`, `platform_profile`, `/etc/nvpmodel.conf` | Info: preflight 뒤에 클럭이나 online CPU를 바꿀 수 있는 것이 있다 |
| `memory-lock` | `/proc/<pid>/status`의 `VmLck` | Warn: 잠긴 메모리가 없다. page fault는 상한 없는 지연이다 |
| `cgroup-cpu` | cgroup을 따라 올라간 `cpu.max` | Warn: quota가 있는데 SCHED_OTHER 스레드(stage 자신이나 reset 자식)가 그 안에서 돈다 |
| `cgroup-memory` | `memory.high`, `memory.max`, `memory.swap.max`, `systemd-oomd` | Info: `memory.high`가 걸려 있거나 oomd가 돈다 |
| `secure-exec` | `/proc/<pid>/auxv`의 `AT_SECURE` | Warn: ld.so가 `LD_PRELOAD`를 버렸다(10절) |
| `bind-now` | 환경의 `LD_BIND_NOW`, 매핑된 모든 객체의 `BIND_NOW` | Info: lazy binding하는 객체가 있다(10절) |

`preflight(spec)`는 stage 하나로는 보이지 않는 것, 즉 파일 전체를 이 호스트에서 판정한다. bind 때는 돌지 않고 `capa check`와 `capa status`가 출력한다.

| id | Fail / Warn / Info |
| --- | --- |
| `housekeeping` | Fail: 격리 CPU와 hard RT worker의 CPU가 online CPU를 전부 덮는다 |
| `foreign-rt-thread` | stage마다 Warn: 파일이 선언하지 않은 SCHED_FIFO나 SCHED_RR 스레드가 stage의 CPU에서 stage 이상의 우선순위로 돌 수 있다. 커널 스레드와 인터럽트 스레드도 센다. `migration/N`은 세지 않는다. 선언됐다는 것은 stage를 쥐었거나, `match`가 가리키거나, external stage의 `expect_priority`로 돌거나, inherit stage의 자식이라는 뜻이다 |
| `deadline-task` | Warn: SCHED_DEADLINE 태스크가 stage CPU에서 돌 수 있다. 모든 FIFO stage보다 위에서 돈다. `sugov:N`은 schedutil의 것이다 |
| `nohz-single-task` | Warn: `nohz_full` CPU 하나에 stage가 둘이다. tick은 실행 가능한 태스크가 하나일 때만 멈춘다 |
| `smt-neighbor` | Warn: hard RT worker 둘이 한 코어의 SMT sibling에 있다 |
| `llc-handoff` | Info: 체인에서 이웃한 stage의 CPU가 last-level cache를 나누지 않는다(`cache/index3`, 없으면 cpufreq `related_cpus`) |
| `taint` | Warn: 커널이 W(경고 발생)나 L(soft lockup)로 tainted다 |

`verify(stage, tid)`는 아무것도 걸지 않는다. capa가 건 뒤 `chrt`, systemd, 다른 코드가 스레드를 옮길 수 있고, 그것을 여기서 본다.

| id | 뜻 |
| --- | --- |
| `observed-thread` | Fail: 스레드를 읽을 수 없다. 사라진 스레드가 쉬는 스레드처럼 읽히면 안 된다 |
| `observed-policy` | 선언한 정책과 스레드의 정책 |
| `observed-priority` | 선언한 priority(또는 `expect_priority`)와 스레드의 priority |
| `observed-children` | 선언한 `children`과 스레드의 `SCHED_RESET_ON_FORK` |
| `observed-cpus` | 스레드의 CPU가 선언한 CPU 안에 있으면 Ok |

파일이 주장하지 않는 항목은 `Unknown`이다. 같은 때 `/proc`에서 finding 셋을 더 읽는다.

| id | 뜻 |
| --- | --- |
| `pi-boosted` | Warn: 스레드가 boost된 채 돈다. 더 높은 스레드가 이 스레드가 쥔 PI lock을 기다린다는 뜻이다. `observed-priority`는 `/proc/<tid>/stat`의 boost된 값이 아니라 정적 우선순위(`sched_getparam`)로 판정한다 |
| `signals` | Warn: RT 스레드인데 프로세스가 SIGINT, SIGTERM, SIGCHLD를 잡고 이 스레드는 막지 않아, 핸들러가 이 스레드에서 돌 수 있다. RT가 아닌 스레드는 Unknown |
| `timerslack` | `timerslack_ns`와 함께 Ok. 읽는 쪽이 그 스레드 자신이거나 CAP_SYS_NICE가 있어야 하고, 아니면 Unknown |

`window`가 0이 아닌 `verify(stage, tid, window)`는 카운터를 `window` 간격으로 두 번 읽어 차이를 판정한다. `window`만큼 잔다.

| id | 소스 | Warn |
| --- | --- | --- |
| `thread-faults` | `/proc/<pid>/task/<tid>/stat`의 `minflt`, `majflt` | RT 스레드에 fault가 났다 |
| `thread-migrations` | `.../sched`의 `se.nr_migrations` | CPU 하나에 고정한 stage가 옮겨졌다 |
| `thread-preemptions` | `.../sched`의 `nr_involuntary_switches` | RT 스레드가 선점됐다. 그 CPU에서 선점할 수 있는 것은 선언된 release stage뿐이어야 한다 |
| `cpu-interrupts` | `/proc/interrupts`, stage의 CPU | tick이 아닌 행이 그 CPU에서 늘었거나, `nohz_full` CPU에서 tick이 초당 10번 넘게 돈다(멈추면 초당 1번쯤이다). 행마다 처방을 적는다. 장치 행은 그 `smp_affinity_list`, RES는 다른 태스크가 그 CPU에서 깨어나는 것, CAL은 다른 CPU의 function call, TLB는 프로세스의 매핑 변경, IWI는 irq_work다 |
| `cpu-softirqs` | `/proc/softirqs`, stage의 CPU | softirq가 그 CPU에서 돌았다. RCU는 `rcu_nocbs`, SCHED는 `isolcpus=domain`, TIMER와 HRTIMER는 `nohz_full`, NET_RX와 BLOCK은 장치 인터럽트 affinity로 푼다 |
| `cgroup-throttle` | 스레드 cgroup을 따라 올라간 `cpu.stat`의 `nr_throttled`, `throttled_usec`, `memory.events`의 `high` | cgroup이 스레드를 묶었다 |

`verify_process(spec)`는 이 프로세스에서 어떤 stage도 설명하지 못하는 RT 스레드마다 `undeclared-rt-thread` Warn을 하나씩 내고, 없으면 Ok 하나를 낸다. stage를 쥔 스레드(`bind`, `spawn`, `apply_checked`)는 설명된다. `children: inherit`인 stage의 스케줄로 도는 스레드도 설명된다. stage의 값과 같게 돌기만 하는 스레드는 capa 밖에서 그렇게 된 것이라 목록에 나온다. Fail이 아니라 Warn인 이유는 선언 안 한 스레드가 잘못인지 capa가 알 수 없기 때문이다. `io-uring`도 보고한다. `iou-sqp-*`, `iou-wrk-*` 스레드가 있으면 Warn이다(io-wq 워커는 SCHED_OTHER로 돌고, `IORING_SETUP_SQ_AFF` 없는 SQPOLL 스레드는 만든 스레드의 CPU를 물려받아 격리 CPU에서도 돈다). `stage-cpu-shared`는 `children_cpus` 없는 hard stage나 `nohz_full` stage가 쥐어져 있고, 그 CPU에 SCHED_OTHER 스레드(reset 자식)가 남아 있으면 Warn이다.

### 다른 프로세스

stage를 쥔 스레드는 기록되어 다른 프로세스가 볼 수 있다. bind하는 프로세스는 기록 하나를 `/dev/shm/capa.<pid>.<starttime>`(`CAPA_RUN_DIR`로 옮길 수 있다)에 두고, 사는 동안 쓰기 락을 쥔다. 프로세스가 끝나면 커널이 락을 푼다. 그래서 읽는 쪽은 그 프로세스에 묻지 않고도 살아 있는 기록과 죽은 기록을 가른다. fork한 자식은 부모의 기록을 살려 두지 않는다.

`/dev/shm`은 모든 사용자가 쓸 수 있다. 그래서 읽는 쪽은 커널이 보증하는 만큼만 기록을 믿는다.

| 기록 | 믿는 조건 |
| --- | --- |
| bind 기록(`CAPA_RUN_DIR`) | 일반 파일이고(링크나 fifo 제외) 소유자만 쓸 수 있다. 소유자가 이름이 가리키는 프로세스의 사용자이고, 그 프로세스가 이름에 적힌 시각에 시작했고, 락이 걸려 있다. 다른 프로세스를 가리키는 줄은 무시한다 |
| 데몬 기록(`CAPA_DAEMON_DIR`, 기본 `/run/capa`) | 같다. 다만 줄이 어느 프로세스든 가리킬 수 있다. 디렉터리가 그것을 보증하므로, 디렉터리는 소유자만 쓸 수 있어야 하고 파일도 그 소유자의 것이어야 한다 |

기록에는 길이와 체크섬이 있다. 다시 쓰는 도중에 읽힌 기록은 반쯤 믿지 않고 다시 읽는다.

- `bindings()`는 이 호스트에서 쥐어진 stage를 전부 준다. `Binding`은 `node`, `group`, `name`, `pid`, `tid`다. 끝난 프로세스의 기록과 끝난 스레드의 줄은 빠진다. 읽기는 아무것도 지우지 않는다. 죽은 기록은 다음에 자기 기록을 만드는 프로세스가 치운다.
- `verify_process(spec, pid)`는 `verify_process(spec)`와 같은 질문을 다른 프로세스에 한다. stage를 쥔 스레드는 그 프로세스의 기록에서 읽는다. 사라진 프로세스는 `observed-process` Fail이다. 어떤 기록(자기 기록, 데몬 기록)도 가리키지 않는 프로세스는 어느 스레드가 stage를 쥐었는지 모르므로 `record` Unknown이 붙는다.
- 기록을 쓰지 못해도 적용은 그대로이고, report에 `record` Unknown이 남는다. 다른 프로세스는 그 스레드를 보지 못한다.

`ThreadState`는 `policy`(raw `SCHED_*` 숫자. `Policy`가 이름 붙이지 않은 클래스일 수 있다), `priority`, `cpus`, `reset_on_fork`다.

## 5. flux `PartitionedExecutor`

`flux::PartitionedExecutor`는 콜백 그룹마다 스레드를 따로 준다. `capa_flux`는 core와 이름이 같은 함수에 executor, 노드, 그룹을 앞에 붙인 것이다. executor는 템플릿 인자다. `capa_flux`는 flux를 링크하지 않으므로 executor의 헤더는 직접 include한다.

함수는 `capa/flux.hpp`에 있다. 그룹당 선언이 하나라서 아래 세 형태는 서로 다른 그룹에 건다.

```cpp doc:flux
capa::flux::bind(ex, *node, grab_group, "grab");  // CAPA_SPEC의 stage
capa::flux::apply_checked(ex, *node, infer_group, opts, capa::Strictness::Hard);  // 값으로
capa::flux::apply_checked(ex, *node, loop_group, spec.stage(name, "loop"));  // Spec의 stage
```

- `group`은 `node`의 것이어야 한다. `nullptr`은 노드의 기본 그룹이다.
- rclcpp 콜백 그룹에는 이름이 없어, 이 호출이 파일의 `(node, label)`과 스레드를 잇는 유일한 연결이다. stage를 넘기면 호출 시점에 대조한다. stage가 이 노드를 가리키는지, 빈 라벨이 기본 그룹과 짝인지, 라벨이 있으면 다른 그룹과 짝인지 본다. `external` stage는 거절한다.
- 그룹당 선언 하나이고, 그 그룹은 `ex`가 맡아야 한다. 두 번째 선언은 호출 시점에 예외를 던지고, 어느 자식도 맡지 않는 그룹의 선언은 `spin()`이 거절한다.
- `spin()`은 SCHED_OTHER 스레드에서 부른다. 자식은 호출 스레드의 스케줄로 시작하므로, RT 호출자 아래에서는 선언 없는 그룹이 그 우선순위로 돈다. `verify_process`가 그런 스레드를 보고한다.
- message_filters synchronizer 하나의 입력은 전부 한 그룹에 둔다. 동기화 정책은 짝이 맞은 콜백을 자기 `std::mutex`를 쥔 채 돌리고, 그 뮤텍스에는 priority inheritance가 없다. 입력이 두 그룹 스레드에 걸치면 두 그룹의 우선순위가 묶인다.
- message_filters 경로에는 할당이 있다(정책 큐, 프레임마다 `shared_ptr` control block). hard 체인에는 놓지 않는다. 필터 없는 flux 배달에는 할당이 없다.

## 6. Python

```python doc:python
import capa
import capa.flux

capa.bind(node.get_fully_qualified_name(), "loop")
t = capa.spawn("/perception_node", "infer", run_inference)
capa.flux.bind(ex, node, group, "grab")

rep = capa.apply_checked(capa.Options(capa.Policy.Fifo, 20, [4]), capa.Strictness.Soft)
if not rep.ok():
    print(rep.to_string())
state = capa.current()          # state.policy는 os.SCHED_FIFO와 비교한다
```

1절부터 5절까지의 이름은 전부 `capa`나 `capa.flux`에 같은 인자로 있다. `Options(policy=, priority=, cpus=, children=)`는 키워드도 받는다. 예외 대응은 아래와 같다.

| C++ | Python |
| --- | --- |
| `std::system_error`(커널 거절) | `OSError` |
| `std::invalid_argument`(다르게 해석될 요청, external stage, 이미 쥔 stage) | `ValueError` |
| `std::runtime_error`(preflight 거절, 되읽기 불일치, 파일 규칙, 없는 라벨) | `RuntimeError` |

Python은 RT 대상이 아니다. 어떤 우선순위를 걸어도 GIL과 GC는 상한 없는 지연원으로 남는다. Python에서 capa가 정하는 것은 여러 스레드가 동시에 runnable일 때 커널이 누구를 고르는지와 각자 어느 코어를 쓰는지다. GIL을 놓는 구간(numpy, 디코딩, I/O)에서 의미가 있다.

## 7. C++와 Python이 다른 곳

| 차이 | 이유 |
| --- | --- |
| `spawn`이 C++에서는 `std::thread`, Python에서는 시작된 `threading.Thread`를 준다 | Python 함수는 Python이 시작한 스레드에서 돌아야 한다 |
| `capa.flux`는 `group`이 `node`의 것인지 확인하지 않는다 | flux Python 그룹은 자기 노드를 모른다. stage의 노드는 `node`와 대조한다 |
| `capa.flux`에서 `group=None`은 노드 단위(`add_node`에 준 노드) | rclpy는 단위를 그룹과 노드로 나눈다. C++에는 그룹만 있다 |
| `capa.flux.apply_checked`는 함수 하나가 `Options`와 `Stage`를 다 받는다 | Python에는 overload가 없다 |

`capa_py/test/test_surface.py`는 공개 이름이 한쪽에만 있으면 실패한다.

## 8. capa가 하지 않는 것

- 타이밍을 보장하지 않는다. 메모리 대역 간섭, GPU 드라이버 순서, 타이머 위상, 센서 간 disparity는 우선순위로 풀리지 않는다. preflight 통과는 호스트 설정이 됐다는 뜻이지 마감을 지킨다는 뜻이 아니다.
- 라이브러리는 코드를 넣을 수 없는 스레드(DDS 내부, SDK 스레드, IRQ 스레드)를 걸지 않는다. `match`를 적은 stage는 데몬 `capad`가 건다. 데몬이 없으면 `external`로 선언하고 `verify`한다.
- 우선순위를 도출하지 않는다. 사람이 적고 파일 규칙이 검사한다.
- RT stage는 주기적으로 block해야 한다. PREEMPT_RT에서 CPU별 타이머와 RCU 스레드(`ktimers/N`, `rcuc/N`)는 FIFO 1이라, 잠들지 않는 stage는 그 CPU에서 이들을 굶긴다. `sched_rt_runtime_us=-1`은 RT throttle을 끈다. 6.12부터는 그래도 fair deadline server가 굶는 SCHED_OTHER 스레드를 위해 RT 태스크에게서 초당 최대 50 ms를 가져간다(`rt-throttle`이 보고한다). 그러나 `ktimers`와 `rcuc`는 SCHED_OTHER가 아니라 FIFO라서, 이들에게 CPU를 되찾아 주는 장치는 없다.
- 지연을 재지 않는다. 재는 방법은 [`latency.ko.md`](latency.ko.md)에 있다.

## 9. flux와의 연결

`capa_flux`와 `capa.flux`는 flux executor의 `on_thread_start` hook을 등록한다. executor를 템플릿 인자(C++)나 아무 객체(Python)로 받으므로 flux를 링크하지 않는다. flux는 capa를 모르고 `capa_core`는 flux에 의존하지 않는다. hook, 템플릿, 패키지 구성은 언제든 바뀔 수 있고, `capa_flux`와 `capa.flux` 밖의 코드는 여기에 기대지 않는다.

## 10. Linux에서 RT 스레드 돌리기

파일은 우선순위끼리를 검사한다. 아래 규칙은 우선순위로 표현할 수 없는 것에 관한 것이다. 여기 적은 finding이 capa가 그 일부를 보는 곳이다.

### 우선순위 사슬 밖의 대기

다른 스레드를 기다리는 스레드는 그 스레드의 스케줄 위치를 물려받는다. lock이면 PI mutex가 쥔 쪽에 기다리는 쪽의 우선순위를 준다. 일을 넘겨받는 대기(futex 워드, eventfd, condition)는 쥔 것이 없으니 아무도 boost되지 않는다. 기다리는 쪽은 넘겨주는 스레드보다 먼저 돌 수 없다. 넘겨주는 스레드를 체인의 `release` stage로 선언하고, RT로 기다리는 쪽보다 위에 둔다. nice만 올리는 것(`WQ_HIGHPRI` workqueue는 nice -20)으로는 부족하다. 그 CPU의 RT 스레드가 여전히 먼저 돈다.

flux를 쓰는 프로세스에는 이런 스레드가 셋 있고, GPU가 있으면 드라이버 인터럽트 스레드가 더해진다.

| 스레드 | 풀어 주는 것 | 선언 |
| --- | --- | --- |
| `flux-parker` | 구독한 입력이 도착했을 때 executor의 대기 | `match: {process: <comm>, thread: flux-parker}` |
| `flux-gpu-wait` | Deferred release 모드에서 GPU fence가 signal된 loan | `match: {process: <comm>, thread: flux-gpu-wait}` |
| `flux-gpu-fd` | dGPU publisher에 대한 subscriber의 attach(fd 교환) | 체인이 도는 동안 attach가 일어날 때만 |
| GPU 드라이버의 인터럽트 스레드 | 그 GPU의 모든 fence 대기(PREEMPT_RT나 `threadirqs`에서 스레드가 된다) | `match: {irq: <handler>}`, 이름은 `/sys/kernel/irq/<n>/actions`의 것 |

```yaml
chains:
  perception:
    target: hard
    stages:
      - name: gpu_irq
        kind: release
        policy: fifo
        priority: 88
        match: {irq: nvidia}
      - name: parker
        kind: release
        policy: fifo
        priority: 86
        match: {process: perception, thread: flux-parker}
      - node: /perception_node
        group: infer
        policy: fifo
        priority: 70
        cpus: [3]
```

`foreign-rt-thread`는 파일이 선언하지 않았는데 stage보다 위에 있는 RT 스레드를 보여 준다. 넘겨주는 스레드가 RT가 아니면 거기 나오지 않는다. stage가 무엇을 기다리는지에서 찾는다.

### PREEMPT_RT의 타이머

PREEMPT_RT에서 타이머는 두 문맥 중 하나에서 만료된다. RT와 DEADLINE 태스크의 nanosleep, futex, epoll timeout은 hard interrupt 문맥에서 만료된다. SCHED_OTHER의 timeout, timerfd, POSIX timer는 FIFO 1인 `ktimers/N`에서 만료된다. 그래서 RT stage가 자기 CPU에서 `ktimers`를 못 돌게 하면, 그 CPU에 있는 SCHED_OTHER executor의 ROS timer와 timerfd 주기 루프가 늦는다. RT stage의 주기 대기는 timerfd가 아니라 `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, ...)`이나 futex timeout으로 한다.

### 우선순위 대역

| 대역 | 용도 |
| --- | --- |
| 99 | 커널의 watchdog과 stopper 작업. stage는 두지 않는다(`priority-band`가 알린다) |
| 85..94 | RT stage를 먹이는 인터럽트 스레드 |
| 80..89 | 주 RT stage |
| 50..79 | 그 밖의 인터럽트 스레드(커널은 50으로 시작한다) |

### `nohz_full`

`nohz_full` CPU는 실행 가능한 태스크가 하나일 때만 tick을 멈춘다. stage는 하나만 두고, 그 stage에 `children_cpus`를 줘서 reset 자식이 떠나게 한다(`nohz-single-task`, `stage-cpu-shared`). tick이 멈췄는지는 `verify`의 window로 본다. 멈췄으면 local timer 인터럽트가 초당 1번쯤이다. `nohz_full` CPU에서는 softlockup watchdog이 기본으로 꺼져 있어, 거기서 잠들지 않는 stage는 커널도 보고하지 않는다.

### 권한

RT 권한은 `RLIMIT_RTPRIO`로 준다. systemd 유닛의 `LimitRTPRIO=`나 `limits.conf`의 `rtprio`다. `setcap cap_sys_nice+ep`로 주지 않는다. file capability가 붙은 `exec`는 secure-exec가 되고, 그러면 ld.so가 `LD_PRELOAD`와 `LD_LIBRARY_PATH`를 말없이 버린다. 미리 올린 tracer(orma, lttng-ust)가 빠진다(`secure-exec`).

### lazy binding

PLT 항목마다 첫 호출은 동적 링커를 거친다. RT 경로에서는 최대 수 ms의 정지다. 프로세스에 `LD_BIND_NOW=1`을 주거나 `-z now`로 링크한다(`bind-now`).

### fork

RT 스레드가 있는 프로세스에서 `fork`하면 mmap_lock을 쥔 채 페이지 테이블을 복사하고 private 페이지를 전부 쓰기 금지로 바꾼다. 그 뒤 각 스레드는 다음 쓰기에서 copy-on-write fault를 겪는다. 잠근 메모리도 마찬가지다. 도우미 프로세스는 체인이 돌기 전에 띄우거나, 주소 공간을 복사하지 않는 `posix_spawn`을 쓴다.
