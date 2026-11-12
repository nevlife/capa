# capa 사용자 API

capa는 데이터 경로에 있는 스레드에 OS 스케줄링(정책, 우선순위, CPU, 자식에게 상속할 설정)을 적용하고, 적용된 값을 확인한다. 스케줄은 파일 하나에 적고, 코드는 스레드가 어느 stage인지만 지정한다.

| 패키지 | 내용 | 의존 |
| --- | --- | --- |
| `capa_core` | C++17, ROS 미사용. 체인 파일, 적용, 호스트 preflight, 되읽기, 확인 | yaml-cpp |
| `capa_ros` | ROS 2 콜백 그룹 스레드용. flux `PartitionedExecutor`에 적용하는 같은 이름의 함수와 컴포넌트 컨테이너 `capa_component_container` | `capa_core`, `rclcpp`, flux(`flux_cpp`, `flux_components`) |
| `capa_py` | Python. `capa`(`capa_core`의 이름), `capa.flux`(`capa/flux.hpp`의 이름), `capa` 명령 | `capa_core` |

C++와 Python은 이름, 인자 순서, 기본값이 같다. 언어 때문에 다른 곳은 7절에 있다.

C++ 함수는 실패를 반환값으로 알린다. 실패 가능한 함수는 `capa::Result<T>`(`capa/result.hpp`)를 반환한다.

- 값은 `*`나 `->`로 읽고, 읽기 전에 `if (r)`이나 `r.ok()`로 확인한다. 실패한 `Result`의 값을 읽으면 동작이 정의되지 않는다.
- 실패 내용은 `r.error()`의 `capa::Error`(`kind`, `what`, `code`)다.
- Python에서는 같은 실패가 예외로 발생한다(6절).
- 이 문서의 "거절한다"는 C++에서는 실패한 `Result`, Python에서는 예외다.

## 1. 스레드마다 한 줄

체인의 모든 stage는 `CAPA_SPEC`이 가리키는 체인 파일에 적는다. `stages` 항목 하나가 stage이고, stage는 스레드 하나(노드와 그룹, 또는 이름)와 그 스케줄(정책, 우선순위, CPU)을 정한다.

```yaml
chains:
  perception_to_control:
    target: hard
    stages:
      - {node: /camera_node, group: grab, policy: fifo, priority: 60, cpus: [2]}
      - {node: /perception_node, group: infer, policy: fifo, priority: 70, cpus: [3]}
      - {node: /control_node, group: loop, policy: fifo, priority: 80, cpus: [4]}
```

스레드는 자기가 어느 stage인지만 지정한다. 사용하는 호출은 스레드를 생성한 주체에 따라 정해진다.

| 스레드 | C++ | Python |
| --- | --- | --- |
| 새로 생성하는 스레드 | `capa::spawn("/perception_node", "infer", fn)` | `capa.spawn("/perception_node", "infer", fn)` |
| 이미 실행 중인 스레드(main, 직접 가진 스레드) | `capa::bind("/control_node", "loop")` | `capa.bind("/control_node", "loop")` |
| flux `PartitionedExecutor` 그룹 스레드 | `capa::flux::bind(ex, *node, group, "grab")` | `capa.flux.bind(ex, node, group, "grab")` |
| 컴포넌트 노드의 그룹 스레드 | 코드 없음. `capa_component_container`가 적용한다(5절) | - |

- `bind(target, group="")`는 호출한 스레드에 그 stage를 적용한다. 적용 전에 호스트를 판정하고, 적용 후에 커널에서 값을 되읽는다. ROS 스레드면 `target`은 노드의 전체 이름(`get_fully_qualified_name()`)이고 `group`은 그룹 라벨이다. `name` stage면 `target`에 그 이름을 주고 그룹은 주지 않는다.
- `spawn(target, group, fn)`은 스레드를 생성해 bind한 뒤 `fn`을 실행하고, stage가 적용된 뒤 반환한다. bind가 거절되면 `fn`은 실행되지 않고 호출자가 거절을 받는다. 새 스레드 안에서 `bind`를 호출하는 대신 이 함수를 사용한다. 새 스레드 안에서 `bind`하면 스레드가 `bind` 줄에 닿을 때까지 스케줄 없이 실행된다.
- `capa::flux::bind(ex, node, group, label="")`는 executor에 bind를 등록한다. `group`을 맡은 자식 스레드가 첫 콜백 전에 그 stage를 자기에게 적용한다. 거절되면 flux가 훅의 실패를 예외로 받으므로 spin이 예외로 종료된다. stage는 호출 시 찾으므로 라벨 오타는 그 자리에서 거절한다.
- 프로세스에 RT 권한이 없어 판정이 `policy-permission` 하나로만 막히면, 세 함수 모두 capad에 위임한다.
  - 기록에 요청을 남기고, 스레드가 선언대로 실행될 때까지 최대 5 s 기다린다. capad가 적용하면 `policy-permission`은 Ok로 바뀐다.
  - 5 s 안에 적용되지 않으면 chain의 target과 관계없이 거절한다. 스레드가 파일과 다른 우선순위로 실행되지 않게 하기 위해서다.
  - capad는 체인 파일에 선언된 값만 적용한다.
- `CAPA_SPEC`이 없으면 세 함수 모두 아무것도 하지 않는다. `bind`는 빈 report를 반환하고, `spawn`은 `fn`만 실행하고, `flux::bind`는 아무것도 등록하지 않는다.
- 파일에 없는 라벨은 거절한다. 아무것도 적용하지 않고 넘어가면 오타를 알아채기 불가능하기 때문이다.
- `bind_declared(target, group="")`는 파일이 그 stage를 선언한 경우에만 `bind`한다. stage가 선언되지 않았거나, external이거나, `match`가 있으면 아무것도 적용하지 않고 `std::nullopt`(Python은 `None`)를 반환한다. 생성하는 스레드마다 이 함수를 호출하는 컨테이너용이다. 라벨을 아는 코드는 `bind`를 사용해야 라벨 오타가 거절로 드러난다.

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
| `node`, `group` | stage | ROS 스레드의 노드 전체 이름과 그룹 라벨 |
| `name` | stage | ROS가 아닌 스레드의 이름. `/`로 시작하지 않고 `group`이 없다 |
| `kind` | stage | `worker`(기본) 또는 `release` |
| `policy`, `priority`, `cpus`, `children` | stage | capa가 적용하는 값(3절) |
| `external`, `expect_priority` | stage | capa가 적용하지 않는 스레드와 그 예상 우선순위 |
| `holds` | stage | `{subscription: <토픽>}` 또는 `{service: <이름>}` |
| `match` | stage | `{irq: <handler>}` 또는 `{process: <comm>, arg: <argv 원소>, thread: <comm>}` |
| `children_cpus` | stage | `capad`가 이 stage의 reset 자식을 이동할 CPU |

- `kind`: `worker`는 계산하고, `release`는 일을 넘겨주기만 한다. IRQ 스레드, DDS 수신 스레드, 타이머 스레드가 release다.
- `external`: `expect_priority`(1..99, 0은 미선언)만 적는다. 스레드에 적용하는 진입점은 이 stage를 전부 거절하고, `verify`로 확인한다.
- `holds`: 이름 없는 콜백 그룹을 그 그룹이 가진 구독이나 서비스로 고른다(5절). 이름은 remap까지 끝난 전체 이름이고, `ros2 node info`가 보여 주는 이름과 같다. `group` 라벨이 있어야 하고, 라벨이 stage의 이름이 된다. external, `match`와 함께 사용하지 않는다.
- `match`: `thread`와, `process`와 `arg` 중 하나 이상이 필요하다. 사용자 코드 삽입이 불가능한 스레드이고, 데몬 `capad`가 찾아서 적용한다. `bind`와 `apply_checked`는 거절한다(`docs/daemon.md`).
- `children_cpus`: `cpus`와 `children: reset`이 있어야 하고 `cpus`와 겹치지 않아야 한다.

capa는 파일을 읽을 때 아래 규칙을 검사하고, 규칙을 어긴 stage를 이름으로 보고한다.

| 규칙 | 검사 |
| --- | --- |
| 흐름 | 한 체인에서 CPU를 공유 가능한 두 worker는 downstream의 우선순위가 upstream과 같거나 높다 |
| release | release stage는 CPU를 공유 가능한 같은 체인의 모든 worker보다 엄격히 높다 |
| 중요도 | 공유 CPU에서 더 중요한 체인의 RT stage는 덜 중요한 체인의 RT stage보다 모두 엄격히 높다 |
| hard 코어 | hard 체인이 적용된 곳에서 RT worker 둘을 한 CPU에 고정하지 않는다 |
| 선언 하나 | 두 체인에 나오는 stage는 두 곳에서 같게 선언한다 |
| 이름 하나 | 두 stage가 같은 이름을 가지지 않는다 |
| 그룹 하나 | 한 노드의 두 stage가 같은 것을 `holds`하지 않는다 |

- 흐름: 같아도 되는 이유는 FIFO가 같은 우선순위끼리 선점하지 않기 때문이다. CPU가 겹치지 않는 stage는 서로 선점하지 않으므로 비교하지 않는다.
- release: release stage가 아래에 있으면 넘겨줄 일이 늦게 전달된다.
- 중요도: 여러 체인에 속한 stage는 그중 가장 높은 중요도를 가진다.
- hard 코어: SCHED_FIFO에는 타임슬라이스가 없어서 한 스레드가 block할 때까지 코어를 양보하지 않는다. release stage는 세지 않는다.
- 선언 하나: strictness는 그중 가장 엄격한 것을 따른다.
- 이름 하나: 노드 `/foo/bar`의 기본 그룹과 노드 `/foo`의 그룹 `bar`는 둘 다 `/foo/bar`다. 메시지, 기록, 데몬은 stage를 이 이름으로 부른다.
- 그룹 하나: 그렇게 하면 두 stage가 한 그룹을 가리키게 된다.
- "CPU를 공유 가능하다"는 `cpus`가 겹친다는 뜻이다. `cpus`가 없는 stage와 external stage는 어느 CPU에서도 실행되므로 모든 CPU를 공유한다.
- RT가 아닌 stage(`policy: other`, `inherit`, `expect_priority: 0`인 external)는 어떤 규칙에도 포함되지 않는다.
- 모르는 키는 거절한다. 버전 필드는 없다. 버전 번호는 스키마 변경을 알릴 뿐 `priorty` 같은 오타는 잡지 못한다.

`capa::Spec::load(path)`는 파일을, `capa::Spec::load()`는 `CAPA_SPEC`을 읽는다. 변수가 없으면 빈 spec이고 `empty()`가 참이다.

| 함수 | 반환 |
| --- | --- |
| `stages()` | stage 전부 |
| `find(target, group)` | stage 하나. 없으면 null |
| `stage(target, group)` | stage 하나의 포인터를 담은 `Result`. 없으면 거절한다 |
| `external()` | external stage만 |
| `declared(node, groups)` | 그룹마다 파일이 선언한 stage 또는 null |

- `declared`는 노드의 콜백 그룹 전부를 `GroupContent`(`is_default`, `subscriptions`, `services`)의 목록으로 받는다. 그룹 없는 stage는 기본 그룹의 것이고, `holds` stage는 그것이 가리키는 구독이나 서비스를 가진 그룹에 속한다. 라벨만 있는 stage, external stage, `match` stage는 찾지 않는다. 한 그룹을 두 stage가 가리키거나, 한 stage가 가리키는 것을 두 그룹이 가지면 추측하지 않고 거절한다.
- `capa::Stage`는 `match`(`irq`, `process`, `arg`, `thread`, `empty()`를 가진 `Match`), `holds`(`subscription`, `service`, `empty()`를 가진 `Holds`), `children_cpus`, `node`, `group`, `name`, `kind`(`Kind::Worker` 또는 `Kind::Release`), `external`, `expect_priority`, `opts`, `strict`, `chain`(처음 선언한 체인)을 담는다.

## 3. 적용

이 절의 함수는 모두 호출한 스레드 자신에게만 적용한다. 다른 스레드에 밖에서 적용하지 않으므로, 스레드가 절반만 설정된 채 실행되는 구간이 생기지 않는다.

함수는 `capa/spec.hpp`에 있고, 이 헤더가 `capa/rt.hpp`를 포함한다.

```cpp doc:apply
capa::Options o;
o.policy = capa::Policy::Fifo;       // Inherit / Other / Fifo / RoundRobin
o.priority = 70;                     // Fifo/RoundRobin이면 1..99, 아니면 0
o.cpus = {3};                        // 비우면 affinity를 안 건드린다
o.children = capa::Children::Reset;  // Reset / Inherit

// 모든 호출이 capa::Result를 돌려준다. 확인한 뒤 * 나 -> 로 값을 읽는다
capa::Result<capa::Report> rep =
  capa::apply_checked(o, capa::Strictness::Hard, /*control_priority=*/80);
if (!rep) {
  std::fprintf(stderr, "%s\n", rep.error().what.c_str());
  return;
}
capa::Result<const capa::Stage *> stage = spec.stage("/perception_node", "infer");
if (stage) {
  rep = capa::apply_checked(**stage);  // Spec의 stage
}
rep = capa::bind("/perception_node", "infer");  // CAPA_SPEC의 stage
```

- `apply_checked(opts, strict=Soft, control_priority=0)`는 `preflight`를 먼저 실행한다. 판정이 적용을 막으면 스레드를 바꾸기 전에 `ErrorKind::Failed`로 거절한다. 통과하면 스레드에 적용하고 커널에서 되읽는다. 되읽은 policy와 priority는 요청과 같아야 하고, 스레드의 CPU는 요청한 CPU 안에 있어야 한다(cpuset이 마스크를 좁힐 가능성이 있다). 어긋나면 거절하며, 이때 스레드는 이미 바뀐 상태다.
- `Strictness`는 `Warn` 처리를 정한다. `Soft`면 Warn을 report로 반환하고, `Hard`면 거절한다. `Fail`은 둘 다 거절한다.
- `apply_checked(stage)`와 `bind`는 stage 전체를 받으므로 체인의 strictness도 함께 받는다. 이때 report의 `priority-order`는 Ok다. 순서는 파일을 읽을 때 검사했기 때문이다.
- 한 stage에는 스레드 하나만 적용한다. 이 프로세스의 살아 있는 스레드가 stage를 점유한 동안 다른 스레드가 같은 stage를 적용하면 `ErrorKind::InvalidArgument`로 거절한다. 점유한 스레드의 재적용은 허용하고, 점유한 스레드가 종료되면 stage가 해제된다.
- 아무것도 요청하지 않는 `Options`는 판정을 포함해 아무것도 하지 않고 빈 report를 반환한다.
- `Options::validate()`는 다르게 해석 가능한 요청을 거절한다. priority 1..99 없이 RT 정책만 준 것, RT 정책 없이 priority를 준 것, RT 정책 없이 `children = Inherit`을 준 것, 커널의 CPU 집합 크기를 넘는 CPU 번호다.
- 커널이 거절하면 `ErrorKind::System`으로 거절하고 `code`에 errno를 담는다. 요청보다 낮은 설정으로 바꿔 적용하지 않는다.

### 자식

Linux는 새 스레드에 부모의 정책, 우선순위, CPU를 상속하고, `fork()`와 `exec()` 뒤에도 유지된다. 그래서 RT 스레드가 라이브러리를 호출하면 그 라이브러리가 생성하는 풀, 로거, `system()`의 하위 프로세스가 전부 RT로 실행된다.

| `children` | 자식의 시작 상태 | 용도 |
| --- | --- | --- |
| `Reset`(기본) | SCHED_OTHER, 우선순위 0 | 라이브러리 풀, 로거, 하위 프로세스. 이 stage로 모델링하지 않은 모든 것 |
| `Inherit` | 부모의 정책과 우선순위 | 이 stage의 일을 하는 fork-join 협력 스레드(OpenMP 팀 스레드 등) |

- `Reset`은 `SCHED_RESET_ON_FORK` 플래그를 설정한다. 이 플래그는 `fork`뿐 아니라 `pthread_create`에도 적용되고, RT 정책일 때만 효과가 있다.
- affinity는 `children` 값과 관계없이 상속된다. 커널에 이를 막는 플래그가 없기 때문이다. 그래서 reset 자식도 stage의 CPU에서 실행되지만, stage 스레드가 잠든 동안에만 실행된다.
- `Reset`으로 적용한 스레드는 `CAP_SYS_NICE` 없이 플래그를 해제 불가능하다. 같은 스레드를 나중에 `Inherit`으로 바꾸려면 그 권한이 필요하고, 권한이 없어 거절되면 메시지에 이 이유가 나온다.
- 오래 실행되는 helper 스레드에 자기 스케줄이 필요하면 따로 stage로 선언한다. helper 안에서 bind하거나 `external`로 선언한다.

## 4. 확인

```cpp doc:check
capa::Result<capa::Report> host = capa::preflight(o, /*control_priority=*/80);  // 판정만
capa::ThreadState self = capa::current();                       // 이 스레드, 커널에서
int tid = capa::this_tid();                                     // 이 스레드의 커널 id
capa::Result<capa::ThreadState> other = capa::observe(tid);     // 아무 스레드, 프로세스 무관
capa::Report drift = capa::verify(stage, tid);                  // 스레드와 stage 대조
capa::Report stray = capa::verify_process(spec);                // 설명되지 않는 RT 스레드
capa::Report whole = capa::preflight(spec);                     // 파일 전체를 이 호스트에서
capa::Report spent = capa::verify(stage, tid, std::chrono::seconds(1));  // 카운터까지
```

`Report`는 `findings`를 담는다. `Finding`은 `id`(바뀌지 않는 슬러그), `verdict`, `detail`이다.

| `Verdict` | 뜻 |
| --- | --- |
| `Ok` | 통과 |
| `Warn` | hard에서는 적용을 막고, soft에서는 보고만 한다 |
| `Fail` | 적용을 막는다 |
| `Unknown` | 소스를 읽지 못했다. 통과가 아니다 |
| `Info` | 알아 둘 사실. 어떤 strictness에서도 적용을 막지 않는다 |

`ok()`는 `Fail`이 없으면 참이다. `find(id)`는 finding 하나를 반환하고, 없으면 null이다. `to_string()`은 사람이 읽는 출력이고 파싱용 형식이 아니다.

`preflight(opts, control_priority=0, strict=Soft)`는 요청 하나를 기준으로 호스트를 판정한다. `strict`는 그 요청을 hard stage로 볼지 soft stage로 볼지 정하고, 아래 finding 몇 개는 hard stage면 Warn, soft stage면 Info다. `apply_checked`와 `bind`는 받은 strictness를 넘긴다.

아래는 `preflight`의 finding이다. 실행되는 finding은 요청에 따라 다르다. 첫 묶음은 RT 정책이 있을 때, 둘째 묶음은 `cpus`가 있을 때 실행되고, 셋째 묶음은 항상 실행된다. 마지막 묶음은 스레드가 속한 프로세스에 관한 것이라 커널 스레드에서는 제외된다. 소스를 읽지 못하면(root 전용 debugfs, lockdown, 다른 사용자의 프로세스) 그 finding은 Unknown이고 Fail이 되지 않는다.

| id | 소스 | Fail / Warn / Info |
| --- | --- | --- |
| `policy-permission` | `CapEff`(CAP_SYS_NICE)와 `/proc/self/limits`의 `RLIMIT_RTPRIO` | Fail: 권한이 없다 |
| `rt-throttle` | `/proc/sys/kernel/sched_rt_runtime_us`, debugfs `sched/fair_server/cpuN`(6.12+) | Warn: throttle이나 fair deadline server가 바쁜 RT 스레드의 CPU를 우선순위와 무관하게 가져간다 |
| `priority-order` | `opts.priority`와 `control_priority` | Fail: 우선순위가 control loop와 같거나 높다 |
| `priority-band` | `opts.priority` | Info: 99는 커널의 stopper와 watchdog 작업의 대역이다(10절) |
| `cpu-online` | `/sys/devices/system/cpu/online` | Fail: 없는 CPU |
| `cpu-isolation` | `/sys/devices/system/cpu/isolated`, 루트 `cpuset.cpus.isolated`, `nohz_full` | Warn: 다른 태스크와 CPU를 공유한다 |
| `cpuset` | 프로세스 cgroup의 `cpuset.cpus.effective`, 루트까지의 `cpuset.cpus.partition` | Fail: cpuset 밖의 CPU(컨테이너, systemd `AllowedCPUs`), Warn: invalid partition |
| `cpu-governor` | `cpufreq/scaling_governor` | Warn: 주파수 상승이 wakeup 지연을 늘린다 |
| `cpufreq` | `scaling_driver`, `energy_performance_preference`, `scaling_min_freq`, `related_cpus`, `cpuinfo_transition_latency`, boost | Warn: EPP가 `performance`가 아니다 |
| `rcu-offload` | `/proc/cmdline`의 `rcu_nocbs`, `nohz_full` | Warn: 커널이 미뤄 둔 메모리 해제(RCU 콜백)가 RT CPU에서 실행된다 |
| `irq-affinity` | `/proc/irq/*/effective_affinity_list` | Warn: 인터럽트 핸들러가 그 CPU의 모든 태스크를 선점한다 |
| `irq-managed` | debugfs `irq/irqs/N` | Warn: managed 인터럽트가 그 CPU로 오고 이동 불가능하다(`isolcpus=managed_irq`) |
| `irq-default-affinity` | `/proc/irq/default_smp_affinity` | Warn: 나중에 생기는 인터럽트(핫플러그 카메라)가 그 CPU에서 시작한다 |
| `irqbalance` | 실행 중인 `irqbalance` | Warn: 약 10 s마다 인터럽트 affinity를 다시 쓴다 |
| `cpu-idle` | `cpuidle/stateN/{latency,disable}`, `power/pm_qos_resume_latency_us` | Warn: 허용된 idle state의 exit latency가 50 us를 넘는다 |
| `workqueue` | `/sys/devices/virtual/workqueue/cpumask` | hard stage면 Warn, soft stage면 Info: unbound 커널 작업이 그 CPU에서 실행된다 |
| `kthread-affinity` | CPU 둘 이상에 허용된 커널 스레드 | hard stage면 Warn, soft stage면 Info: 그 CPU에서 실행 가능한 커널 스레드가 있다 |
| `smt` | `topology/thread_siblings_list` | hard stage면 Warn, soft stage면 Info: SMT sibling이 격리되지 않았다 |
| `cpu-capacity` | `cpu_capacity` | Info: capacity가 낮은 코어(효율 코어) |
| `softlockup-watchdog` | `kernel.watchdog`, `kernel.watchdog_cpumask` | hard stage면 Warn, soft stage면 Info: 4 s softlockup 타이머가 그 CPU에서 실행된다 |
| `kernel-preemption` | `/sys/kernel/realtime`, `/proc/version` | Warn: PREEMPT_RT가 아니다 |
| `preempt-mode` | `/proc/version`, debugfs `sched/preempt` | Warn: `none`, `voluntary`, RT가 아닌 stage의 `lazy` |
| `sched-ext` | `/sys/kernel/sched_ext/state` | Warn: BPF 스케줄러가 이 stage나 그 reset 자식을 맡는다 |
| `thp` | `transparent_hugepage/{enabled,defrag}` | Warn: defrag가 `always`(fault 경로에서 compaction 실행), Info: enabled가 `always` |
| `compaction` | `vm.compact_unevictable_allowed` | Info: 1이라 compaction이 mlock된 페이지를 이동한다 |
| `numa-balancing` | `node/online`, `kernel.numa_balancing` | Info: 노드가 둘 이상인데 활성화되어 있다 |
| `dirty-limits` | `vm.dirty_bytes`, `vm.dirty_ratio`, `MemTotal` | Info: dirty 한도가 비율로 설정되어 있다 |
| `audit` | 부트 라인의 `audit=`, `auditd` | Info: audit가 활성화되어 있다 |
| `console` | `/proc/consoles`, `kernel.printk` | Info: PREEMPT_RT가 아닌 커널에 시리얼 콘솔이 있다 |
| `clocksource` | `current_clocksource` | Warn: `hpet` 또는 `acpi_pm`(읽기 한 번에 약 1 us), Info: `tsc`, `arch_sys_counter`, `kvm-clock`가 아닌 다른 clocksource |
| `virtualization` | CPU의 `hypervisor` 플래그, `/sys/hypervisor` | Info: 가상 머신이다(측정한 지연은 실제 하드웨어와 무관) |
| `power-policy` | `power-profiles-daemon`, `tuned`, `thermald`, `platform_profile`, `/etc/nvpmodel.conf` | Info: preflight 뒤에 클럭이나 online CPU를 바꿀 가능성이 있는 것이 있다 |
| `memory-lock` | `/proc/<pid>/status`의 `VmLck` | Warn: 잠긴 메모리가 없어 page fault 시 지연에 상한이 없다 |
| `cgroup-cpu` | cgroup을 따라 올라간 `cpu.max` | Warn: quota 안에서 SCHED_OTHER 스레드(stage 자신이나 reset 자식)가 실행된다 |
| `cgroup-memory` | `memory.high`, `memory.max`, `memory.swap.max`, `systemd-oomd` | Info: `memory.high`가 설정되어 있거나 oomd가 실행 중이다 |
| `secure-exec` | `/proc/<pid>/auxv`의 `AT_SECURE` | Warn: ld.so가 `LD_PRELOAD`를 버렸다(10절) |
| `bind-now` | 환경의 `LD_BIND_NOW`, 매핑된 모든 객체의 `BIND_NOW` | Info: lazy binding하는 객체가 있다(10절) |

- `policy-permission`: seccomp, LSM 라벨, `hidepid`가 설정되어 있으면 detail에 적는다.
- `rt-throttle`: 6.12+에서 `-1`인데 fair server를 읽지 못하면 Unknown이다.
- `priority-order`: `control_priority`가 0이면 Unknown이다.
- `cpuset`: cgroup v2가 없으면(JetPack 5) Unknown이다.
- `cpufreq`: active 모드 intel_pstate와 amd-pstate에서는 governor가 아니라 EPP가 주파수를 정한다.
- `preempt-mode`: debugfs를 읽지 못하면 Unknown이다.
- `dirty-limits`: 한도를 넘겨 쓰는 스레드는 잠든다.
- `audit`: 부트 라인과 `auditd` 둘 다 정보가 없으면 Unknown이다.

`preflight(spec)`는 파일 전체를 이 호스트에서 판정해 stage 하나만으로는 보이지 않는 문제를 찾는다. bind 시에는 실행되지 않고, `capa check`와 `capa status`가 결과를 출력한다.

| id | Fail / Warn / Info |
| --- | --- |
| `housekeeping` | Fail: 격리 CPU와 hard RT worker의 CPU가 online CPU를 전부 덮는다 |
| `foreign-rt-thread` | stage마다 Warn: 파일이 선언하지 않은 SCHED_FIFO나 SCHED_RR 스레드가 stage의 CPU에서 stage 이상의 우선순위로 실행 가능하다 |
| `deadline-task` | Warn: SCHED_DEADLINE 태스크가 stage CPU에서 실행 가능하다 |
| `nohz-single-task` | Warn: `nohz_full` CPU 하나에 stage가 둘이다 |
| `smt-neighbor` | Warn: hard RT worker 둘이 한 코어의 SMT sibling에 있다 |
| `llc-handoff` | Info: 체인에서 이웃한 stage의 CPU가 last-level cache를 공유하지 않는다(`cache/index3`, 없으면 cpufreq `related_cpus`) |
| `taint` | Warn: 커널이 W(경고 발생)나 L(soft lockup)로 tainted다 |

- `foreign-rt-thread`는 커널 스레드와 인터럽트 스레드도 세고, `migration/N`은 세지 않는다. 선언된 스레드는 stage를 점유한 스레드, `match`가 가리키는 스레드, external stage의 `expect_priority`로 실행되는 스레드, inherit stage의 자식이다.
- `deadline-task`: DEADLINE 태스크는 모든 FIFO stage보다 우선한다. `sugov:N`은 schedutil governor의 태스크다.
- `nohz-single-task`: tick은 실행 가능한 태스크가 하나일 때만 멈춘다.

`verify(stage, tid)`는 아무것도 적용하지 않는다. capa가 적용한 뒤에도 `chrt`, systemd, 다른 코드가 스레드의 스케줄을 바꿀 가능성이 있고, `verify`는 그런 변화를 찾는다.

| id | 뜻 |
| --- | --- |
| `observed-thread` | Fail: 스레드를 읽지 못했다 |
| `observed-policy` | 선언한 정책과 스레드의 정책 |
| `observed-priority` | 선언한 priority(또는 `expect_priority`)와 스레드의 priority |
| `observed-children` | 선언한 `children`과 스레드의 `SCHED_RESET_ON_FORK` |
| `observed-cpus` | 스레드의 CPU가 선언한 CPU 안에 있으면 Ok |

`observed-thread`가 Fail인 것은 사라진 스레드를 쉬는 스레드로 잘못 읽지 않기 위해서다. 파일이 선언하지 않은 항목은 `Unknown`이다. `verify`는 같은 호출에서 `/proc`을 읽어 finding 셋을 더 낸다.

| id | 뜻 |
| --- | --- |
| `pi-boosted` | Warn: 스레드가 boost된 상태로 실행된다 |
| `signals` | RT 스레드가 SIGINT, SIGTERM, SIGCHLD를 받는 경로(아래) |
| `timerslack` | Ok이고 `timerslack_ns` 값을 함께 적는다 |

- `pi-boosted`는 더 높은 스레드가 이 스레드가 보유한 PI lock을 기다린다는 뜻이다. `observed-priority`는 `/proc/<tid>/stat`의 boost된 값이 아니라 정적 우선순위(`sched_getparam`)로 판정한다.
- `timerslack`: 읽는 쪽이 그 스레드 자신이거나 CAP_SYS_NICE가 있어야 하고, 아니면 Unknown이다.

`signals`는 RT 스레드에 대해, 프로세스에 handler가 있고 이 스레드가 막지 않은 SIGINT, SIGTERM, SIGCHLD를 판정한다.

- 커널은 프로세스에 온 시그널을 정해진 스레드에 먼저 전달한다. `kill`과 터미널이 보낸 시그널은 첫 스레드(tid가 pid인 스레드)가, SIGCHLD는 그 자식을 fork한 스레드가 먼저 받는다.
- 다른 스레드는 그 스레드가 시그널을 막았거나, 종료 중이거나, 멈췄거나, 이미 대기 중인 시그널을 둔 채 잠든 경우에만 받는다.
- Warn: 이 스레드가 첫 스레드다. 첫 스레드가 그 시그널을 막거나 이미 종료됐다. SIGCHLD에 handler가 있다(fork한 스레드가 막으면 임의의 스레드가 받고, `system()`이 막는다).
- Info: SIGINT, SIGTERM을 첫 스레드가 먼저 받는다.
- RT가 아닌 스레드는 Unknown이다.

`verify(stage, tid, window)`에 0이 아닌 `window`를 주면 카운터를 `window` 간격으로 두 번 읽어 차이를 판정하고, 호출은 `window`만큼 잠든다.

| id | 소스 | Warn |
| --- | --- | --- |
| `thread-faults` | `/proc/<pid>/task/<tid>/stat`의 `minflt`, `majflt` | RT 스레드에 fault가 발생했다 |
| `thread-migrations` | `.../sched`의 `se.nr_migrations` | CPU 하나에 고정한 stage가 이동됐다 |
| `thread-preemptions` | `.../sched`의 `nr_involuntary_switches` | RT 스레드가 선점됐다 |
| `cpu-interrupts` | `/proc/interrupts`, stage의 CPU | tick이 아닌 행이 그 CPU에서 늘었거나, `nohz_full` CPU에서 tick이 초당 10번을 넘는다 |
| `cpu-softirqs` | `/proc/softirqs`, stage의 CPU | softirq가 그 CPU에서 실행됐다 |
| `cgroup-throttle` | 스레드 cgroup을 따라 올라간 `cpu.stat`의 `nr_throttled`, `throttled_usec`, `memory.events`의 `high` | cgroup이 스레드를 제한했다 |

- `thread-preemptions`: 그 CPU에서 선점 가능한 것은 선언된 release stage뿐이어야 한다.
- `cpu-interrupts`: tick이 멈추면 초당 약 1번이다. 행마다 고칠 곳을 적는다. 장치 행은 그 `smp_affinity_list`, RES는 다른 태스크가 그 CPU에서 깨어나는 것, CAL은 다른 CPU의 function call, TLB는 프로세스의 매핑 변경, IWI는 irq_work다.
- `cpu-softirqs`: RCU는 `rcu_nocbs`, SCHED는 `isolcpus=domain`, TIMER와 HRTIMER는 `nohz_full`, NET_RX와 BLOCK은 장치 인터럽트 affinity로 해결한다.

`verify_process(spec)`는 이 프로세스에서 어떤 stage로도 설명되지 않는 RT 스레드마다 `undeclared-rt-thread` Warn을 하나씩 낸다. 그런 스레드가 없으면 Ok 하나를 낸다.

- 설명되는 스레드는 stage를 점유한 스레드(`bind`, `spawn`, `apply_checked`)와 `children: inherit`인 stage의 스케줄로 실행되는 스레드다.
- stage와 값만 같은 스레드는 capa 밖에서 설정된 것이므로 목록에 나온다.
- Fail이 아니라 Warn인 이유는 선언하지 않은 스레드가 잘못인지 capa가 판단 불가능하기 때문이다.
- `io-uring`: `iou-sqp-*`나 `iou-wrk-*` 스레드가 있으면 Warn이다. io-wq 워커는 SCHED_OTHER로 실행되고, `IORING_SETUP_SQ_AFF` 없는 SQPOLL 스레드는 생성한 스레드의 CPU를 상속받아 격리 CPU에서도 실행된다.
- `stage-cpu-shared`: `children_cpus` 없는 hard stage나 `nohz_full` stage를 어떤 스레드가 점유했는데, 그 CPU에 SCHED_OTHER 스레드(reset 자식)가 남아 있으면 Warn이다.

### 다른 프로세스

어느 스레드가 stage를 점유했는지는 기록으로 남으므로 다른 프로세스도 확인 가능하다.

- bind하는 프로세스는 기록 하나를 `/dev/shm/capa.<pid>.<starttime>`(`CAPA_RUN_DIR`로 변경 가능)에 저장하고, 살아 있는 동안 그 파일의 쓰기 락을 유지한다.
- 프로세스가 종료되면 커널이 락을 해제한다. 그래서 읽는 쪽은 그 프로세스에 묻지 않고 살아 있는 프로세스와 종료된 프로세스의 기록을 구별한다.
- fork한 자식이 살아 있어도 부모의 기록은 살아 있는 것으로 보이지 않는다.

`/dev/shm`은 모든 사용자가 쓰기 가능하므로, 읽는 쪽은 커널이 보증하는 만큼만 기록을 신뢰한다. bind 기록(`CAPA_RUN_DIR`)은 아래 조건을 모두 만족해야 신뢰한다.

- 일반 파일이다(링크나 fifo 제외).
- 소유자만 쓰기 가능하다.
- 소유자가 파일 이름이 가리키는 프로세스의 사용자다.
- 그 프로세스가 이름에 적힌 시각에 시작했다.
- 락이 유지되고 있다.

다른 프로세스를 가리키는 줄은 무시한다. 데몬 기록(`CAPA_DAEMON_DIR`, 기본 `/run/capa`)도 조건은 같지만, 줄이 어느 프로세스든 가리키는 것이 허용된다. 이것은 디렉터리로 보증하므로, 디렉터리는 소유자만 쓰기 가능해야 하고 파일도 그 소유자의 것이어야 한다.

기록에는 길이와 체크섬이 있다. 읽는 쪽은 다시 쓰는 도중에 읽은 기록을 버리고 다시 읽는다.

- `bindings()`는 이 호스트에서 점유된 stage를 전부 반환한다. `Binding`의 필드는 `node`, `group`, `name`, `pid`, `tid`다. 종료된 프로세스의 기록과 종료된 스레드의 줄은 제외된다. 읽기는 아무것도 지우지 않고, 종료된 프로세스의 기록은 다음에 자기 기록을 만드는 프로세스가 지운다.
- `verify_process(spec, pid)`는 `verify_process(spec)`와 같은 검사를 다른 프로세스에 한다. stage를 점유한 스레드는 그 프로세스의 기록에서 읽는다. 프로세스가 사라졌으면 `observed-process` Fail이다. 어떤 기록(그 프로세스 자신의 기록, 데몬 기록)도 가리키지 않는 프로세스는 점유 스레드를 확인 불가능하므로 `record`가 Unknown이다.
- 기록 쓰기에 실패해도 스레드에 적용한 설정은 유지하고, report에 `record` Unknown을 남긴다. 이 경우 다른 프로세스는 그 스레드를 보지 못한다.

`ThreadState`의 필드는 `policy`, `priority`, `cpus`, `reset_on_fork`다. `policy`는 raw `SCHED_*` 숫자이고, `Policy`에 이름이 없는 스케줄링 클래스일 가능성도 있다.

## 5. flux `PartitionedExecutor`

`flux::PartitionedExecutor`는 콜백 그룹마다 스레드를 따로 생성한다. `capa_ros`의 `capa::flux` 함수는 core의 같은 이름 함수 앞에 executor, 노드, 그룹 인자를 추가한 형태다. executor는 템플릿 인자이고, executor의 헤더는 사용하는 쪽에서 직접 include한다.

rclcpp executor는 그룹마다 스레드를 정하지 않고 스레드 시작 훅도 없어서 그룹별 스케줄을 적용 불가능하다. 그래서 capa는 flux executor에 적용하고, 방법은 둘이다.

| 방법 | 사용하는 경우 | 그룹 지정 |
| --- | --- | --- |
| `capa::flux::bind` | executor를 생성하는 `main()`을 직접 작성하는 경우 | 호출에 준 라벨 |
| `capa_component_container` | 노드를 launch로 한 컨테이너에 묶는 경우 | 기본 그룹 stage와 `holds`(아래 "컨테이너") |

컨테이너에서는 `main()`이 컨테이너의 것이라 `bind`를 호출 불가능하고, flux 컨테이너는 capa를 호출하지 않는다.

함수는 `capa/flux.hpp`에 있다. 그룹 하나에는 선언이 하나만 가능하므로, 아래 세 호출은 서로 다른 그룹에 적용한다.

```cpp doc:flux
capa::Result<void> done = capa::flux::bind(ex, *node, grab_group, "grab");  // CAPA_SPEC의 stage
if (!done) {
  std::fprintf(stderr, "%s\n", done.error().what.c_str());
  return;
}
done = capa::flux::apply_checked(ex, *node, infer_group, opts, capa::Strictness::Hard);  // 값으로
capa::Result<const capa::Stage *> stage = spec.stage(name, "loop");
if (stage) {
  done = capa::flux::apply_checked(ex, *node, loop_group, **stage);  // Spec의 stage
}
```

- `group`은 `node`의 것이어야 한다. `nullptr`은 노드의 기본 그룹이다.
- rclcpp 콜백 그룹에는 이름이 없으므로 파일의 `(node, label)`과 스레드를 잇는 것은 이 호출뿐이다. stage를 넘기면 호출 시 대조한다. 대조하는 것은 stage가 이 노드를 가리키는지, 빈 라벨이 기본 그룹과 짝인지, 라벨이 있으면 다른 그룹과 짝인지다. `external` stage는 거절한다.
- 그룹 하나에 선언은 하나이고, 그 그룹은 `ex`가 맡는 그룹이어야 한다. 같은 그룹을 두 번 선언하면 호출 시 거절하고, 어느 자식 스레드도 맡지 않는 그룹을 선언하면 `spin()`이 거절한다.
- `spin()`은 SCHED_OTHER 스레드에서 호출한다. 자식 스레드는 `spin()`을 호출한 스레드의 스케줄로 시작하므로, RT 스레드가 `spin()`을 호출하면 선언 없는 그룹도 그 우선순위로 실행된다. `verify_process`가 그런 스레드를 보고한다.
- message_filters synchronizer 하나의 입력은 전부 한 그룹에 넣는다. 동기화 정책은 자기 `std::mutex`를 보유한 채 짝이 맞은 콜백을 실행하는데, 이 뮤텍스에는 priority inheritance가 없다. 입력이 두 그룹의 스레드에 걸치면 두 그룹의 우선순위가 서로 묶인다.
- message_filters 경로는 메모리를 할당한다(정책 큐, 프레임마다 `shared_ptr` control block). 그래서 이 경로는 hard 체인에 넣지 않는다. 필터를 거치지 않는 flux 전달은 할당하지 않는다.

### 컨테이너

컴포넌트 컨테이너는 노드를 로드해 그룹마다 스레드를 생성하지만, 그 노드의 그룹 라벨을 모른다. 그래서 파일의 기본 그룹 stage와 `holds` stage로 그룹을 고른다. 그룹 생성 순서대로 번호를 매겨 고르지 않는다. 그렇게 하면 노드 코드에 그룹이 하나 늘었을 때 같은 번호가 경고 없이 다른 그룹을 가리키게 된다.

```yaml
chains:
  plan_to_control:
    target: soft
    stages:
      - node: /planning/motion_velocity_planner
        group: trajectory   # stage의 이름
        holds: {subscription: /planning/path_optimizer/trajectory}
        policy: fifo
        priority: 44
```

`capa_component_container`는 flux 컴포넌트 컨테이너(`flux::ComponentManager`, 그룹마다 스레드 하나)에 이 규칙을 더한 실행 파일이다. 노드 코드는 capa를 호출하지 않아도 된다.

```bash
CAPA_SPEC=chain.yaml ros2 run capa_ros capa_component_container --ros-args -r __node:=planning_container
```

| 시점 | 동작 |
| --- | --- |
| 노드 load | 노드의 그룹 전부를 넘겨 `Spec::declared`를 호출한다 |
| 그룹 스레드 시작 | 그 그룹의 stage를 찾아 첫 콜백 전에 `apply_checked`한다 |
| 거절 | spin이 멈추고, 컨테이너는 stage 이름과 이유를 출력하고 종료 코드 1로 종료된다 |
| `CAPA_SPEC` 없음 | 아무것도 적용하지 않고 시작 시 info 로그 한 줄을 남긴다 |

- 노드 load: 한 그룹을 두 stage가 가리키거나 한 stage가 가리키는 것을 두 그룹이 가지면 load를 거절하고, 이유는 응답의 `error_message`에 있다. 이 노드의 stage 중 어느 그룹에도 대응하지 않는 것은 stage마다 warn 로그를 한 줄 남긴다. 그런 stage는 가리키는 것을 가진 그룹이 없는 `holds`(오타, 바뀐 토픽)와 라벨만 있는 stage(컨테이너 안에는 라벨을 지정하는 코드가 없다)다.
- 그룹 스레드 시작: 적용된 그룹마다 info 로그 한 줄(stage 이름, tid, Warn 판정의 id)을 남긴다. stage가 없는 그룹은 변경하지 않는다. 매니저 자신의 그룹과 load 뒤에 생긴 그룹도 같은 규칙이다.
- 거절: 기준은 `apply_checked`와 같다. soft 체인은 Fail에서, hard 체인은 Warn과 Fail에서 거절한다. `capa::flux::bind`가 거절될 때 `spin()`이 오류를 내는 것과 같은 방식이고, 그래서 파일과 다른 스케줄로 실행되는 스레드가 남지 않는다.
- 파일이 규칙에 어긋나면 컨테이너가 시작하지 않는다. 파일은 시작 시 한 번 읽는다.
- 프로세스 이름(comm)은 매니저 노드 이름의 앞 15 B다. 그래서 `capad`의 `match: {process: ...}`로 컨테이너마다 다른 스레드를 선택 가능하다.
- `holds: {subscription: ...}`는 rclcpp 구독과, `Subscription::add_to_callback_group`으로 그룹에 넣은 flux 구독을 본다. flux `message_filters::Subscriber`와 agnocast 구독은 보지 못한다.

훅을 직접 등록하는 컨테이너는 아래 함수를 사용한다.

```cpp doc:container
capa::Result<void> done = capa::flux::bind_declared(ex, *node);  // 파일이 선언한 그룹 전부
// hook이 없는 executor:
capa::Result<const capa::Stage *> st = capa::flux::declared(spec, *node, group);
if (st && *st != nullptr) {
  capa::Result<capa::Report> rep = capa::apply_checked(**st);  // 그룹의 스레드에서
}
```

- `bind_declared(ex, node)`는 `node`(`NodeBaseInterface`)의 그룹 중 `Spec::declared`가 stage를 찾은 그룹마다 bind를 등록한다. 로드한 노드마다 `spin()` 전에 한 번 호출한다. 호출 뒤에 생긴 그룹에는 등록하지 않는다. 라벨만 있는 stage는 적용하지 않으며, 그런 stage는 `bind`로 적용한다. `CAPA_SPEC`이 없으면 아무것도 등록하지 않는다.
- `declared(spec, node, group)`는 그룹 하나의 stage를 반환하고, 없으면 null이다. 노드의 그룹 전부를 보고 판정하므로 `Spec::declared`와 결과가 같다. `group`이 `node`의 것이 아니면 거절한다. 훅이 없는 executor를 사용하면 그룹의 스레드에서 이 함수를 호출하고, 받은 stage로 `apply_checked`한다. 노드의 그룹 목록 락을 획득하므로 `for_each_callback_group` 안에서 호출하지 않는다.
- `content(node, group)`는 그룹이 가진 것을 반환한다. rclcpp 구독의 토픽 이름, 서비스 이름, `add_to_callback_group`으로 넣은 flux 구독의 토픽 이름이다. flux `message_filters::Subscriber`와 agnocast 구독은 여기에 없으므로, 그런 구독을 가진 호출자는 토픽을 더해 `Spec::declared`를 직접 호출한다.

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

1절부터 5절까지의 이름은 전부 `capa`나 `capa.flux`에 같은 인자로 있다. `Options(policy=, priority=, cpus=, children=)`는 키워드 인자도 받는다. C++ `ErrorKind`와 Python 예외의 대응은 다음과 같다.

| C++ `ErrorKind` | Python |
| --- | --- |
| `System`(커널 거절) | `OSError` |
| `InvalidArgument`(다르게 해석될 요청, external stage, 이미 점유된 stage) | `ValueError` |
| `Failed`(preflight 거절, 되읽기 불일치, 파일 규칙, 없는 라벨) | `RuntimeError` |

Python은 RT 대상이 아니다. 어떤 우선순위를 적용해도 GIL과 GC 때문에 지연에 상한이 없다. Python에서 capa가 정하는 것은 여러 스레드가 동시에 runnable일 때 커널이 고르는 스레드와 각 스레드가 사용하는 코어이고, GIL을 해제하는 구간(numpy, 디코딩, I/O)에서 의미가 있다.

## 7. C++와 Python이 다른 곳

| 차이 | 이유 |
| --- | --- |
| 실패를 C++에서는 `capa::Result`로, Python에서는 예외로 알린다 | 예외는 Python이 실패를 알리는 표준 방식이다 |
| `spawn`이 C++에서는 `std::thread`, Python에서는 시작된 `threading.Thread`를 반환한다 | Python 함수는 Python이 시작한 스레드에서 실행되어야 한다 |
| `capa.flux`는 `group`이 `node`의 것인지 확인하지 않는다 | flux Python 그룹은 자기 노드를 모르며, stage의 노드는 `node`와 대조한다 |
| `capa.flux`에서 `group=None`은 노드 단위(`add_node`에 준 노드) | rclpy는 단위를 그룹과 노드로 나누고, C++에는 그룹만 있다 |
| `capa.flux.apply_checked`는 함수 하나가 `Options`와 `Stage`를 모두 받는다 | Python에는 overload가 없다 |
| `capa.flux.bind_declared(ex, node)`는 노드 단위 하나에 적용하고, 노드의 rclpy 그룹 둘에 stage가 선언되어 있으면 `ValueError`다 | rclpy executor는 노드의 ROS 쪽 그룹을 전부 노드 스레드 하나에서 실행한다 |
| 컴포넌트 컨테이너(`capa_component_container`)는 C++에만 있다 | rclpy에는 컴포넌트 컨테이너가 없다 |

flux Python 그룹은 라벨로 `bind`한다. flux Python 그룹의 구독은 executor만 알기 때문이다. `capa_py/test/test_surface.py`는 공개 이름이 한쪽에만 있으면 실패한다.

## 8. capa가 하지 않는 것

- 타이밍을 보장하지 않는다. 메모리 대역 간섭, GPU 드라이버 순서, 타이머 위상, 센서 간 disparity는 우선순위로 해결되지 않는다. preflight 통과는 호스트가 설정됐다는 뜻이지 마감을 지킨다는 뜻이 아니다.
- 라이브러리는 코드 삽입이 불가능한 스레드(DDS 내부, SDK 스레드, IRQ 스레드)에 적용하지 않는다. `match`를 적은 stage는 데몬 `capad`가 적용하고, 데몬이 없으면 `external`로 선언하고 `verify`한다.
- 우선순위를 정해 주지 않는다. 우선순위는 사람이 적고, capa는 파일 규칙으로 검사한다.
- 지연을 측정하지 않는다. 측정 방법은 [`latency.md`](latency.md)에 있다.

RT stage는 주기적으로 block해야 한다.

- PREEMPT_RT에서 CPU별 타이머와 RCU 스레드(`ktimers/N`, `rcuc/N`)는 FIFO 1이다. 잠들지 않는 stage가 있으면 그 CPU에서 이들이 실행되지 못한다.
- `sched_rt_runtime_us=-1`은 RT throttle을 비활성화한다. 6.12부터는 그래도 fair deadline server가 실행되지 못하는 SCHED_OTHER 스레드를 위해 RT 태스크에게서 초당 최대 50 ms를 가져간다(`rt-throttle`이 보고한다).
- `ktimers`와 `rcuc`는 SCHED_OTHER가 아니라 FIFO이므로, 이들에게 CPU를 되돌려 주는 메커니즘은 없다.

## 9. flux와의 연결

- `capa_ros`와 `capa.flux`는 flux executor의 `on_thread_start` 훅을 등록한다.
- `capa_ros`는 flux를 링크한다. 그룹이 가진 flux 구독을 읽어야 하고, 컨테이너가 `flux::ComponentManager`의 하위 클래스이기 때문이다.
- `capa.flux`는 executor를 임의의 객체로 받으므로 flux를 import하지 않는다.
- flux는 capa를 모르고, `capa_core`는 flux에 의존하지 않는다.
- 훅, 템플릿, 패키지 구성은 계약에 포함되지 않는다. `capa_ros`와 `capa.flux` 밖의 코드는 이 구성에 의존하지 않는다.

## 10. Linux에서 RT 스레드 실행하기

파일 규칙은 우선순위 사이의 관계만 검사한다. 아래 규칙은 우선순위로 표현 불가능한 것을 다룬다. capa가 finding으로 확인하는 항목은 그 finding을 함께 적었다.

### 우선순위 사슬 밖의 대기

다른 스레드를 기다리는 스레드는 그 스레드의 스케줄 위치를 상속받는다.

- 기다리는 대상이 lock이면 PI mutex가 lock을 보유한 스레드에 기다리는 스레드의 우선순위를 준다.
- 일을 넘겨받으려고 기다리는 경우(futex 워드, eventfd, condition)는 보유한 lock이 없으므로 아무도 boost되지 않고, 기다리는 스레드는 넘겨주는 스레드보다 먼저 실행 불가능하다.
- 넘겨주는 스레드를 체인의 `release` stage로 선언하고, RT로 기다리는 쪽보다 높은 우선순위를 준다.
- nice만 올리는 것(`WQ_HIGHPRI` workqueue는 nice -20)으로는 부족하다. 그 CPU의 RT 스레드가 여전히 먼저 실행된다.

flux를 사용하는 프로세스에는 이런 스레드가 셋 있고, GPU가 있으면 드라이버의 인터럽트 스레드도 있다.

| 스레드 | 대기를 해제하는 대상 | 선언 |
| --- | --- | --- |
| `flux-parker` | 구독한 입력 도착 시 executor의 대기 | `match: {process: <comm>, thread: flux-parker}` |
| `flux-gpu-wait` | Deferred release 모드에서 GPU 완료 신호가 온 loan | `match: {process: <comm>, thread: flux-gpu-wait}` |
| `flux-gpu-fd` | dGPU 발행자에 대한 구독자의 attach(fd 교환) | 체인 실행 중 attach가 발생하는 경우에만 |
| GPU 드라이버의 인터럽트 스레드 | 그 GPU의 모든 완료 신호 대기(PREEMPT_RT나 `threadirqs`에서 스레드가 된다) | `match: {irq: <handler>}`, 이름은 `/sys/kernel/irq/<n>/actions`의 것 |

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

`foreign-rt-thread`는 파일이 선언하지 않았는데 stage보다 높은 RT 스레드를 보여 준다. 넘겨주는 스레드가 RT가 아니면 여기에 나오지 않으므로, stage가 무엇을 기다리는지 추적해 찾는다.

### PREEMPT_RT의 타이머

PREEMPT_RT에서 타이머는 두 문맥 중 하나에서 만료된다.

| 대기 | 만료되는 문맥 |
| --- | --- |
| RT와 DEADLINE 태스크의 nanosleep, futex, epoll timeout | hard interrupt 문맥 |
| SCHED_OTHER의 timeout, timerfd, POSIX timer | FIFO 1인 `ktimers/N` |

그래서 RT stage가 자기 CPU에서 `ktimers`의 실행을 막으면, 그 CPU에 있는 SCHED_OTHER executor의 ROS timer와 timerfd 주기 루프가 늦어진다. RT stage의 주기 대기는 timerfd가 아니라 `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, ...)`이나 futex timeout으로 한다.

### 우선순위 대역

| 대역 | 용도 |
| --- | --- |
| 99 | 커널의 watchdog과 stopper 작업. stage에는 사용하지 않는다(`priority-band`) |
| 85..94 | RT stage에 일을 넘기는 인터럽트 스레드 |
| 80..89 | 주 RT stage |
| 50..79 | 그 밖의 인터럽트 스레드(커널 기본값 50) |

### `nohz_full`

`nohz_full` CPU는 실행 가능한 태스크가 하나일 때만 tick을 멈춘다.

- stage를 하나만 배치하고, 그 stage에 `children_cpus`를 줘서 reset 자식이 다른 CPU로 이동하게 한다(`nohz-single-task`, `stage-cpu-shared`).
- tick이 멈췄는지는 `verify`의 window로 확인한다. 멈췄으면 local timer 인터럽트가 초당 약 1번이다.
- `nohz_full` CPU에서는 softlockup watchdog이 기본으로 비활성화되어 있어서, 잠들지 않는 stage가 있어도 커널이 보고하지 않는다.

### 권한

RT 권한은 `RLIMIT_RTPRIO`로 준다. systemd 유닛의 `LimitRTPRIO=`나 `limits.conf`의 `rtprio`로 설정한다. `setcap cap_sys_nice+ep`로 주지 않는다. file capability가 설정된 `exec`는 secure-exec가 되어 ld.so가 `LD_PRELOAD`와 `LD_LIBRARY_PATH`를 경고 없이 버리고, 미리 로드하는 tracer(orma, lttng-ust)가 로드되지 않는다(`secure-exec`).

### lazy binding

PLT 항목마다 첫 호출은 동적 링커를 거치므로, RT 경로에서 이 첫 호출이 최대 수 ms 동안 멈출 가능성이 있다. 프로세스에 `LD_BIND_NOW=1`을 주거나 `-z now`로 링크한다(`bind-now`).

### fork

RT 스레드가 있는 프로세스에서 `fork`하면 커널이 mmap_lock을 보유한 채 페이지 테이블을 복사하고 private 페이지를 전부 쓰기 금지로 바꾼다. 그 뒤 각 스레드는 다음 쓰기에서 copy-on-write fault를 겪고, 잠근 메모리도 마찬가지다. 도우미 프로세스는 체인 실행 전에 시작하거나, 주소 공간을 복사하지 않는 `posix_spawn`으로 시작한다.
