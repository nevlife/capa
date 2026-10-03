# cyra 사용자 API

cyra는 데이터 경로 위 스레드의 OS 스케줄링(정책·우선순위·CPU, 자식에게 물려줄 것)을 걸고 확인함. 스케줄은 파일 하나에 두고, 코드는 스레드가 어느 stage인지만 말함.

| 패키지 | 내용 | 의존 |
| --- | --- | --- |
| `cyra_core` | C++17, ROS 없음. 체인 파일, 적용, 호스트 preflight, 되읽기, 확인 | yaml-cpp |
| `cyra_flux` | flux `PartitionedExecutor` 스레드용으로 같은 함수 | `cyra_core`, `flux_cpp` |
| `cyra_py` | Python. `cyra`(`cyra_core`의 이름)와 `cyra.flux`(`cyra_flux`의 이름) | `cyra_core` |

C++와 Python은 이름, 인자 순서, 기본값이 같음. 다른 곳은 언어가 강제하는 곳뿐이고 7절에 모음.

## 1. 스레드마다 한 줄

파일이 체인의 모든 stage와 그 스케줄을 적음. `CYRA_SPEC`이 그 파일을 가리킴.

```yaml
chains:
  perception_to_control:
    target: hard
    stages:
      - {node: /camera_node, group: grab, policy: fifo, priority: 60, cpus: [2]}
      - {node: /perception_node, group: infer, policy: fifo, priority: 70, cpus: [3]}
      - {node: /control_node, group: loop, policy: fifo, priority: 80, cpus: [4]}
```

스레드는 자기가 어느 stage인지만 말함. 어떤 호출을 쓰는지는 스레드를 누가 만들었는지로 정해짐.

| 스레드 | C++ | Python |
| --- | --- | --- |
| 새로 만드는 스레드 | `cyra::spawn("/perception_node", "infer", fn)` | `cyra.spawn("/perception_node", "infer", fn)` |
| 이미 도는 스레드(main, 직접 가진 스레드) | `cyra::bind("/control_node", "loop")` | `cyra.bind("/control_node", "loop")` |
| flux `PartitionedExecutor` 그룹 스레드 | `cyra::flux::bind(ex, *node, group, "grab")` | `cyra.flux.bind(ex, node, group, "grab")` |

- `bind(target, group="")`는 호출한 스레드를 그 stage에 걸고, 호스트를 판정하고, 커널에서 되읽음. `target`은 노드의 전체 이름(`get_fully_qualified_name()`)과 그룹 라벨, 또는 `name` stage의 이름(그룹 없음).
- `spawn(target, group, fn)`은 스레드를 만들어 bind하고 그 뒤에 `fn`을 돌림. 스레드가 걸린 뒤에 반환. bind가 거절되면 `fn`은 돌지 않고 거절이 호출자에게 던져짐. 스레드 안에서 `bind`를 부르는 방식보다 이것을 씀. 그 방식은 `bind` 줄에 닿을 때까지 스케줄 없이 돎.
- `cyra::flux::bind(ex, node, group, label="")`는 executor에 bind를 등록함. `group`을 맡은 자식이 첫 콜백 전에 자기에게 걸고, 거절은 `spin()`의 에러가 됨. stage는 호출 시점에 찾으므로 오타는 그 자리에서 예외를 던짐.
- `CYRA_SPEC`이 없으면 셋 다 아무것도 안 함. `bind`는 빈 report를 돌려주고, `spawn`은 `fn`만 돌리고, `flux::bind`는 아무것도 등록하지 않음.
- 파일에 없는 라벨은 예외를 던짐. 아무것도 안 걸고 넘어가면 오타가 숨음.

## 2. 체인 파일

```yaml
chains:
  perception_to_control:
    target: hard                  # hard: preflight Warn이 적용을 막는다. soft: 보고만 한다
    criticality: 2                # 높을수록 공유 CPU에서 먼저. 기본 0
    stages:
      - name: camera_usb_irq      # ROS 스레드가 아니면 name으로 부른다
        kind: release             # 일을 넘겨주기만 한다. 먹이는 worker보다 위에서 돈다
        external: true            # cyra가 걸지 않는다. 기본값 50에서 chrt로 올렸다
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
| `name` | stage | 그 밖의 스레드. `/`로 시작하지 않고 `group`을 갖지 않음 |
| `kind` | stage | `worker`(기본)는 계산함. `release`는 일을 넘겨주기만 함. IRQ 스레드, DDS 수신 스레드, 타이머 스레드 |
| `policy`, `priority`, `cpus`, `children` | stage | cyra가 거는 것(3절) |
| `external`, `expect_priority` | stage | cyra가 걸지 않는 스레드. `expect_priority`(1..99, 0은 미선언)만 적음. 거는 진입점은 전부 거절하고 `verify`가 확인함 |
| `match` | stage | `{irq: <handler>}` 또는 `{process: <comm>, thread: <comm>}`. 우리 코드를 넣을 수 없는 스레드로, 데몬 `cyrad`가 찾아서 걺. `bind`와 `apply_checked`는 거절함(`docs/ko/daemon.ko.md`) |
| `children_cpus` | stage | `cyrad`가 이 stage의 reset 자식을 옮길 곳. `cpus`, `children: reset`이 있어야 하고 `cpus`와 겹치지 않아야 함 |

규칙은 파일을 읽을 때 검사함. 어긴 stage를 이름으로 보고함.

| 규칙 | 검사 |
| --- | --- |
| 흐름 | 한 체인에서 CPU를 공유할 수 있는 두 worker는 하류가 상류와 같거나 높음. 같으면 됨. FIFO는 같은 우선순위를 선점하지 않음. CPU가 겹치지 않는 stage는 서로 선점하지 않으므로 비교하지 않음 |
| release | release stage는 CPU를 공유할 수 있는 같은 체인의 모든 worker보다 엄격히 높음. 아래에 있으면 넘겨줄 일이 늦게 풀림 |
| 중요도 | 공유 CPU에서 더 중요한 체인의 RT stage는 덜 중요한 체인의 RT stage보다 전부 엄격히 높음. 여러 체인에 속한 stage는 그중 가장 높은 중요도를 가짐 |
| hard 코어 | hard 체인이 걸린 곳에서 RT worker 둘을 한 CPU에 핀하지 않음. SCHED_FIFO에는 타임슬라이스가 없어 하나가 block할 때까지 코어를 쥠. release stage는 세지 않음 |
| 선언 하나 | 두 체인에 나오는 stage는 두 곳에서 같게 선언함. strictness는 그중 가장 엄격한 것 |
| 이름 하나 | 두 stage가 같은 이름이 되지 않음. 노드 `/foo/bar`의 기본 그룹과 노드 `/foo`의 그룹 `bar`는 둘 다 `/foo/bar`. 메시지, 기록, 데몬은 stage를 이 이름으로 부름 |

- "CPU를 공유할 수 있다"는 `cpus`가 겹친다는 뜻. `cpus`가 없는 stage와 external stage는 어디에나 올 수 있으므로 모든 CPU를 공유함.
- RT가 아닌 stage(`policy: other`, `inherit`, `expect_priority: 0`인 external)는 어떤 규칙에도 들어가지 않음.
- 모르는 키는 거절함. 버전 필드는 없음. 버전 번호는 스키마 교체를 알릴 뿐 `priorty` 같은 오타를 못 잡음.
- `cyra::Spec::load(path)`는 파일을, `cyra::Spec::load()`는 `CYRA_SPEC`을 읽음. 변수가 없으면 빈 spec이고 `empty()`가 참. `stages()`는 전부, `find(target, group)`는 하나 또는 null, `stage(target, group)`는 하나 또는 예외, `external()`은 external만 줌.
- `cyra::Stage`는 `match`(`irq`, `process`, `thread`, `empty()`를 가진 `Match`), `children_cpus`, `node`, `group`, `name`, `kind`(`Kind::Worker` 또는 `Kind::Release`), `external`, `expect_priority`, `opts`, `strict`, `chain`(처음 선언한 체인)을 담음.

## 3. 적용

여기 있는 것은 전부 호출한 스레드 자신에게만 걺. 밖에서 스레드를 걸지 않으므로, 다른 스레드가 절반만 설정한 구간이 생기지 않음.

함수는 `cyra/spec.hpp`에 있고, 이 헤더가 `cyra/rt.hpp`를 포함함.

```cpp doc:apply
cyra::Options o;
o.policy = cyra::Policy::Fifo;       // Inherit / Other / Fifo / RoundRobin
o.priority = 70;                     // Fifo/RoundRobin이면 1..99, 아니면 0
o.cpus = {3};                        // 비우면 affinity를 안 건드린다
o.children = cyra::Children::Reset;  // Reset / Inherit

cyra::Report rep = cyra::apply_checked(o, cyra::Strictness::Hard, /*control_priority=*/80);
rep = cyra::apply_checked(spec.stage("/perception_node", "infer"));  // Spec의 stage
rep = cyra::bind("/perception_node", "infer");                       // CYRA_SPEC의 stage
```

- `apply_checked(opts, strict=Soft, control_priority=0)`는 `preflight`를 먼저 돌림. 판정이 막으면 스레드를 건드리기 전에 `std::runtime_error`를 던짐. 그다음 걸고 커널에서 되읽음. policy와 priority는 같아야 하고, 스레드의 CPU는 요청한 CPU 안에 있어야 함(cpuset이 마스크를 좁힐 수 있음). 어긋나면 예외를 던짐. 이때 스레드는 이미 바뀐 상태.
- `Strictness`가 `Warn`을 읽는 방법을 정함. `Soft`는 report로 돌려주고 `Hard`는 거절함. `Fail`은 둘 다 거절함.
- `apply_checked(stage)`와 `bind`는 stage를 통째로 받아 체인의 strictness가 함께 옴. report의 `priority-order`는 Ok. 순서는 파일을 읽을 때 이미 검사됨.
- 한 stage는 한 스레드. 이 프로세스의 살아 있는 스레드 하나가 stage를 쥐고 있는 동안 다른 스레드가 걸면 `std::invalid_argument`를 던짐. 쥔 스레드가 다시 거는 것은 허용. 쥔 스레드가 끝나면 풀림.
- 아무것도 요청하지 않는 `Options`는 판정까지 포함해 아무것도 안 하고 빈 report를 돌려줌.
- `Options::validate()`는 다르게 해석될 요청을 거절함. 대상: priority 1..99가 없는 RT 정책, RT 정책 없는 priority, RT 정책 없는 `children = Inherit`, 커널 집합 크기를 넘는 CPU 번호.
- 커널의 거절은 `std::system_error`. 조용한 다운그레이드는 없음.

### 자식

Linux는 새 스레드에 부모의 정책, 우선순위, CPU를 줌. `fork()`와 `exec()`도 그대로 유지함. RT 스레드가 라이브러리를 부르면, 그 라이브러리가 만드는 풀, 로거, `system()`에 RT가 전부 샘.

| `children` | 자식의 시작 상태 | 쓰는 곳 |
| --- | --- | --- |
| `Reset`(기본) | SCHED_OTHER, 우선순위 0 | 라이브러리 풀, 로거, 하위 프로세스. 이 stage로 모델링하지 않은 모든 것 |
| `Inherit` | 부모의 정책과 우선순위 | 이 stage의 일을 하는 fork-join 협력 스레드. OpenMP 팀 스레드 등 |

- `Reset`은 `SCHED_RESET_ON_FORK`. `fork`뿐 아니라 `pthread_create`에도 먹고, RT 정책일 때만 효과가 있음.
- affinity는 어느 쪽이든 물려받음. 커널에 막는 플래그가 없음. 그래서 reset 자식도 stage의 CPU에서 돎. stage 스레드가 잠들 때만 돎.
- `Reset`으로 건 스레드는 `CAP_SYS_NICE` 없이는 플래그를 끌 수 없음. 같은 스레드를 나중에 `Inherit`으로 바꾸려면 그 권한이 필요하고, 거절 메시지가 이 이유를 말함.
- 자기 스케줄이 필요한 오래 사는 helper는 따로 stage로 선언함. helper 안에서 bind하거나 `external`로 선언함.

## 4. 확인

```cpp doc:check
cyra::Report host = cyra::preflight(o, /*control_priority=*/80);  // 판정만, 변경 없음
cyra::ThreadState self = cyra::current();                          // 이 스레드, 커널에서
int tid = cyra::this_tid();                                        // 이 스레드의 커널 id
cyra::ThreadState other = cyra::observe(tid);                      // 아무 스레드, 프로세스 무관
cyra::Report drift = cyra::verify(stage, tid);                     // 스레드와 stage 대조
cyra::Report stray = cyra::verify_process(spec);                   // 설명되지 않는 RT 스레드
```

`Report`는 `findings`를 담음. `Finding`은 `id`(안정 슬러그), `verdict`, `detail`. `Verdict`는 `Ok`, `Warn`, `Fail`, `Unknown`이고, `Unknown`은 소스를 읽지 못한 것이지 통과가 아님. `ok()`는 "`Fail` 없음". `find(id)`는 finding 하나나 null을 줌. `to_string()`은 사람이 읽는 것이고 파싱할 형식이 아님.

`preflight` finding:

| id | 소스 | Fail / Warn |
| --- | --- | --- |
| `policy-permission` | `CapEff`(CAP_SYS_NICE)와 `RLIMIT_RTPRIO` | Fail: 권한 없음 |
| `rt-throttle` | `/proc/sys/kernel/sched_rt_runtime_us` | Warn: throttle이 바쁜 RT 스레드의 CPU를 우선순위와 무관하게 뺏음 |
| `priority-order` | `opts.priority`와 `control_priority` | Fail: control loop 이상. `control_priority`가 0이면 Unknown |
| `cpu-online` | `/sys/devices/system/cpu/online` | Fail: 없는 CPU |
| `cpu-isolation` | `/sys/devices/system/cpu/isolated`, `nohz_full` | Warn: 다른 태스크와 CPU를 나눠 씀 |
| `rcu-offload` | `/proc/cmdline`의 `rcu_nocbs`, `nohz_full` | Warn: 미뤄둔 커널 해제가 RT CPU에서 돎 |
| `irq-affinity` | `/proc/irq/*/effective_affinity_list` | Warn: 인터럽트 핸들러가 그 CPU의 모든 태스크를 선점함 |
| `cpu-governor` | `cpufreq/scaling_governor` | Warn: 주파수 상승이 wakeup 지연을 늘림 |
| `kernel-preemption` | `/sys/kernel/realtime`, `/proc/version` | Warn: PREEMPT_RT가 아님 |
| `memory-lock` | `/proc/self/status`의 `VmLck` | Warn: 잠긴 메모리 없음. page fault는 상한 없는 지연 |

`verify(stage, tid)`는 아무것도 걸지 않음. cyra가 건 뒤 `chrt`, systemd, 다른 코드가 스레드를 옮길 수 있고, 그것을 여기서 봄.

| id | 뜻 |
| --- | --- |
| `observed-thread` | Fail: 스레드를 읽을 수 없음. 사라진 스레드가 쉬는 스레드처럼 읽히면 안 됨 |
| `observed-policy` | 선언한 정책과 스레드의 정책 |
| `observed-priority` | 선언한 priority(또는 `expect_priority`)와 스레드의 priority |
| `observed-children` | 선언한 `children`과 스레드의 `SCHED_RESET_ON_FORK` |
| `observed-cpus` | 스레드의 CPU가 선언한 CPU 안에 있으면 Ok |

파일이 주장하지 않는 항목은 `Unknown`.

`verify_process(spec)`는 이 프로세스에서 어떤 stage도 설명하지 못하는 RT 스레드마다 `undeclared-rt-thread` Warn을 하나씩 내고, 없으면 Ok 하나를 냄. stage를 쥔 스레드(`bind`, `spawn`, `apply_checked`)는 설명됨. `children: inherit`인 stage의 스케줄로 도는 스레드도 설명됨. stage의 값과 같게 돌기만 하는 스레드는 cyra 밖에서 그렇게 된 것이라 목록에 나옴. Fail이 아니라 Warn인 이유는 선언 안 한 스레드가 잘못인지 cyra가 알 수 없기 때문.

### 다른 프로세스

stage를 쥔 스레드는 기록되어 다른 프로세스가 볼 수 있음. bind하는 프로세스는 기록 하나를 `/dev/shm/cyra.<pid>.<starttime>`(`CYRA_RUN_DIR`로 옮길 수 있음)에 두고, 사는 동안 쓰기 락을 쥠. 프로세스가 끝나면 커널이 락을 풂. 그래서 읽는 쪽은 그 프로세스에 묻지 않고도 살아 있는 기록과 죽은 기록을 가름. fork한 자식은 부모의 기록을 살려 두지 않음.

`/dev/shm`은 모든 사용자가 쓸 수 있음. 그래서 읽는 쪽은 커널이 보증하는 만큼만 기록을 믿음.

| 기록 | 믿는 조건 |
| --- | --- |
| bind 기록(`CYRA_RUN_DIR`) | 일반 파일이고(링크나 fifo가 아님) 소유자만 쓸 수 있음. 소유자가 이름이 가리키는 프로세스의 사용자이고, 그 프로세스가 이름에 적힌 시각에 시작했고, 락이 걸려 있음. 다른 프로세스를 가리키는 줄은 무시함 |
| 데몬 기록(`CYRA_DAEMON_DIR`, 기본 `/run/cyra`) | 같음. 다만 줄이 어느 프로세스든 가리킬 수 있음. 디렉터리가 그것을 보증하므로, 디렉터리는 소유자만 쓸 수 있어야 하고 파일도 그 소유자의 것이어야 함 |

기록에는 길이와 체크섬이 있음. 다시 쓰는 도중에 읽힌 기록은 반쯤 믿지 않고 다시 읽음.

- `bindings()`는 이 호스트에서 쥐어진 stage를 전부 줌. `Binding`은 `node`, `group`, `name`, `pid`, `tid`. 끝난 프로세스의 기록과 끝난 스레드의 줄은 빠짐. 읽기는 아무것도 지우지 않음. 죽은 기록은 다음에 자기 기록을 만드는 프로세스가 치움.
- `verify_process(spec, pid)`는 `verify_process(spec)`와 같은 질문을 다른 프로세스에 함. stage를 쥔 스레드는 그 프로세스의 기록에서 읽음. 사라진 프로세스는 `observed-process` Fail. 어떤 기록(자기 기록, 데몬 기록)도 가리키지 않는 프로세스는 어느 스레드가 stage를 쥐었는지 모르므로 `record` Unknown이 붙음.
- 기록을 쓰지 못해도 적용은 그대로이고, report에 `record` Unknown이 남음. 다른 프로세스는 그 스레드를 보지 못함.

`ThreadState`는 `policy`(raw `SCHED_*` 숫자. `Policy`가 이름 붙이지 않은 클래스일 수 있음), `priority`, `cpus`, `reset_on_fork`.

## 5. flux `PartitionedExecutor`

`flux::ros::PartitionedExecutor`는 콜백 그룹마다 스레드를 따로 줌. `cyra_flux`는 core와 이름이 같은 함수에 executor, 노드, 그룹을 앞에 붙인 것.

함수는 `cyra/flux.hpp`에 있음. 그룹당 선언이 하나라서 아래 세 형태는 서로 다른 그룹에 걺.

```cpp doc:flux
cyra::flux::bind(ex, *node, grab_group, "grab");  // CYRA_SPEC의 stage
cyra::flux::apply_checked(ex, *node, infer_group, opts, cyra::Strictness::Hard);  // 값으로
cyra::flux::apply_checked(ex, *node, loop_group, spec.stage(name, "loop"));  // Spec의 stage
```

- `group`은 `node`의 것이어야 함. `nullptr`은 노드의 기본 그룹.
- rclcpp 콜백 그룹에는 이름이 없어, 이 호출이 파일의 `(node, label)`과 스레드를 잇는 유일한 연결. stage를 넘기면 호출 시점에 대조함. stage가 이 노드를 가리키는지, 빈 라벨이 기본 그룹과 짝인지, 라벨이 있으면 다른 그룹과 짝인지 봄. `external` stage는 거절함.
- 그룹당 선언 하나이고, 그 그룹은 `ex`가 맡아야 함. 두 번째 선언은 호출 시점에 예외를 던지고, 어느 자식도 맡지 않는 그룹의 선언은 `spin()`이 거절함.
- `spin()`은 SCHED_OTHER 스레드에서 부름. 자식은 호출 스레드의 스케줄로 시작하므로, RT 호출자 아래에서는 선언 없는 그룹이 그 우선순위로 돎. `verify_process`가 그런 스레드를 보고함.
- message_filters synchronizer 하나의 입력은 전부 한 그룹에 둠. 동기화 정책은 짝이 맞은 콜백을 자기 `std::mutex`를 쥔 채 돌리고, 그 뮤텍스에는 priority inheritance가 없음. 입력이 두 그룹 스레드에 걸치면 두 그룹의 우선순위가 묶임.
- message_filters 경로는 할당이 있음(정책 큐, 프레임마다 `shared_ptr` control block). hard 체인에는 놓지 않음. 필터 없는 flux 배달에는 할당이 없음.

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
state = cyra.current()          # state.policy는 os.SCHED_FIFO와 비교한다
```

1절부터 5절까지의 이름은 전부 `cyra`나 `cyra.flux`에 같은 인자로 있음. `Options(policy=, priority=, cpus=, children=)`는 키워드도 받음. 예외 대응은 아래와 같음.

| C++ | Python |
| --- | --- |
| `std::system_error`(커널 거절) | `OSError` |
| `std::invalid_argument`(다르게 해석될 요청, external stage, 이미 쥔 stage) | `ValueError` |
| `std::runtime_error`(preflight 거절, 되읽기 불일치, 파일 규칙, 없는 라벨) | `RuntimeError` |

Python은 RT 대상이 아님. 어떤 우선순위를 걸어도 GIL과 GC는 상한 없는 지연원으로 남음. Python에서 cyra가 정하는 것은 여러 스레드가 동시에 runnable일 때 커널이 누구를 고르는지와 각자 어느 코어를 쓰는지. GIL을 놓는 구간(numpy, 디코딩, I/O)에서 의미가 있음.

## 7. C++와 Python이 다른 곳

| 차이 | 이유 |
| --- | --- |
| `spawn`이 C++에서는 `std::thread`, Python에서는 시작된 `threading.Thread`를 줌 | Python 함수는 Python이 시작한 스레드에서 돌아야 함 |
| `cyra.flux`는 `group`이 `node`의 것인지 확인하지 않음 | flux Python 그룹은 자기 노드를 모름. stage의 노드는 `node`와 대조함 |
| `cyra.flux`에서 `group=None`은 노드 단위(`add_ros_node`에 준 노드) | rclpy는 단위를 그룹과 노드로 나눔. C++에는 그룹만 있음 |
| `cyra.flux.apply_checked`는 함수 하나가 `Options`와 `Stage`를 다 받음 | Python에는 overload가 없음 |

`cyra_py/test/test_surface.py`는 공개 이름이 한쪽에만 있으면 실패함.

## 8. cyra가 하지 않는 것

- 타이밍을 보장하지 않음. 메모리 대역 간섭, GPU 드라이버 순서, 타이머 위상, 센서 간 disparity는 우선순위로 풀리지 않음. preflight 통과는 호스트 설정이 됐다는 뜻이지 마감을 지킨다는 뜻이 아님.
- 라이브러리는 코드를 넣을 수 없는 스레드(DDS 내부, SDK 스레드, IRQ 스레드)를 걸지 않음. `match`를 적은 stage는 데몬 `cyrad`가 걺. 데몬이 없으면 `external`로 선언하고 `verify`함.
- 우선순위를 도출하지 않음. 사람이 적고 파일 규칙이 검사함.
- RT stage는 주기적으로 block해야 함. PREEMPT_RT에서 CPU별 타이머와 RCU 스레드(`ktimers/N`, `rcuc/N`)는 FIFO 1이라, 잠들지 않는 stage는 그 CPU에서 이들을 굶김. `sched_rt_runtime_us=-1`이면 CPU를 되찾아 주는 장치도 없음.

## 9. flux와의 연결

`cyra_flux`와 `cyra.flux`는 flux를 링크하고 executor의 `on_thread_start` hook을 등록함. flux는 cyra를 모르고 `cyra_core`는 flux에 의존하지 않음. hook, 직접 링크, 패키지 구성은 언제든 바뀔 수 있고, `cyra_flux`와 `cyra.flux` 밖의 코드는 여기에 기대지 않음.
