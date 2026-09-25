# cyra 사용자 API

cyra는 데이터 경로 위 스레드의 OS 스케줄링(정책·우선순위·CPU)을 걸고 확인한다. 패키지는 셋이다.

| 패키지 | 내용 | 의존 |
| --- | --- | --- |
| `cyra_core` | C++17, ROS 없음. 스레드 스케줄링, 호스트 preflight, 되읽기, 체인 선언 파일 | yaml-cpp |
| `cyra_py` | `cyra_core`의 Python 바인딩(`cyra`)과 `cyra.flux` | `cyra_core` |
| `cyra_flux` | flux `PartitionedExecutor` 스레드용 C++ 스케줄링 | `cyra_core`, `flux_cpp` |

## 1. 스레드 스케줄링 (C++)

```cpp
#include "cyra/rt.hpp"

cyra::Options o;
o.policy = cyra::Policy::Fifo;   // Inherit / Other / Fifo / RoundRobin
o.priority = 80;                 // Fifo/RoundRobin일 때 1..99
o.cpus = {3};                    // 비우면 affinity를 안 건드린다

cyra::Report rep = cyra::preflight(o, /*control_priority=*/90);  // 판정만, 변경 없음
cyra::apply(o);                  // 호출한 스레드에 적용. 거부되면 throw
cyra::ThreadState st = cyra::current();                 // 커널에서 되읽기
cyra::ThreadState other = cyra::observe(cyra::this_tid());  // 남의 스레드도 같은 값

rep = cyra::apply_checked(o, cyra::Strictness::Hard, /*control_priority=*/90);
```

- `apply`는 호출한 스레드 자신에게 건다. all-or-nothing이다. 커널이 거부하면 되돌리고 `std::system_error`를 던진다. 조용한 다운그레이드는 없다.
- `preflight`는 아무것도 안 바꾸고 finding 목록(`rep.ok()`, `rep.to_string()`)으로 왜 안 되는지를 보고한다.
- `apply_checked`는 그 둘을 잇는다. `preflight`를 먼저 돌리고 판정이 막으면 스레드를 건드리기 전에 `std::runtime_error`를 던진다. 커널은 마감을 못 지키는 호스트에서도 요청을 받아주므로, `apply`만 쓰면 "성공"이 "실시간으로 돈다"를 뜻하지 않는다.
- `Strictness`가 `Warn`을 어떻게 읽을지 정한다. `Soft`는 감수하고 report로 돌려주며, `Hard`는 거절한다. `Fail`은 둘 다 거절한다. 기본은 `Soft`다.
- 아무것도 요청하지 않는 `Options`는 `apply_checked`에서도 완전한 no-op이다. 판정할 대상이 없다.
- `current`는 자기 스레드를, `observe(tid)`는 남의 스레드를 커널에서 읽는다. 거는 것은 자기 스레드만 되지만 읽는 것은 프로세스를 넘어서도 된다. `tid`는 `this_tid()`가 준다. `std::thread::id`는 커널이 아는 이름이 아니다. 읽을 수 없는 스레드는 `std::system_error`다.

한 줄 요약 대신 항목을 직접 보려면 `rep.findings`를 순회하거나 `rep.find(id)`로 하나를 집는다.

```cpp
for (const cyra::Finding & f : rep.findings) {
  if (f.verdict == cyra::Verdict::Fail) {
    log(f.id, f.detail);
  }
}
const cyra::Finding * one = rep.find("cpu-online");   // 안 봤으면 nullptr
```

`Finding`은 셋이다. `id`(안정 슬러그), `verdict`, `detail`(왜 그 판정인지). `Verdict`는 `Ok`·`Warn`·`Fail`·`Unknown`이고 `ok()`는 "`Fail` 없음"이다. `Unknown`은 커널이 소스를 안 내준 것이지 통과가 아니다.

| finding id | 소스 | Fail/Warn의 뜻 |
| --- | --- | --- |
| `policy-permission` | `CapEff`(CAP_SYS_NICE) + `RLIMIT_RTPRIO` | Fail: 권한 없음. `apply`가 EPERM으로 던진다 |
| `rt-throttle` | `/proc/sys/kernel/sched_rt_runtime_us` | Warn: throttle이 busy RT 스레드를 우선순위와 무관하게 preempt한다 |
| `priority-order` | opts.priority vs `control_priority` | Fail: transport가 control 이상. transport는 control을 절대 preempt하지 않는다 |
| `cpu-online` | `/sys/devices/system/cpu/online` | Fail: 없는 코어. `apply`가 EINVAL로 던진다 |
| `cpu-isolation` | `/sys/devices/system/cpu/isolated`·`nohz_full` | Warn: pinning은 마이그레이션만 없앤다. 격리 없는 코어는 남과 공유된다 |
| `rcu-offload` | `/proc/cmdline`의 `rcu_nocbs` + `/sys/devices/system/cpu/nohz_full` | Warn: 미뤄둔 커널 해제가 RT 코어에서 돈다. 시각도 길이도 안 묶인다 |
| `irq-affinity` | `/proc/irq/*/effective_affinity_list` | Warn: 핸들러는 RT 우선순위와 무관하게 그 코어의 모든 태스크를 선점한다 |
| `cpu-governor` | `cpufreq/scaling_governor` | Warn: DVFS ramp가 wakeup 지연을 늘린다. RT 코어는 performance |
| `kernel-preemption` | `/sys/kernel/realtime`·`/proc/version` | Warn: PREEMPT_RT 아님. 커널 내부 구간의 지연 상한이 없다 |
| `memory-lock` | `/proc/self/status`의 `VmLck` | Warn: 잠긴 메모리 없음. page fault는 상한 없는 지연이다 |

## 2. 체인 선언 파일 (C++)

체인 하나의 스케줄링을 파일 하나에 적고 각 노드가 뜰 때 자기 몫을 찾아 적용한다. 노드가 launch로 각각 뜨고 죽어도 성립한다. 미는 쪽이 없고 각자 읽기 때문이다.

```yaml
chains:
  perception_to_control:
    target: hard                # Warn을 막는다. soft면 기록만
    stages:
      - node: /camera_node
        group: dds_listener
        external: true          # cyra가 안 건다. rmw 리스너 같은 남의 스레드
        expect_priority: 60
      - node: /camera_node      # group 생략 = 노드의 기본 콜백 그룹
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

cyra::Spec spec = cyra::Spec::load();   // CYRA_SPEC. 없으면 빈 spec
const std::string name = node->get_fully_qualified_name();
if (!spec.empty()) {
  std::thread worker([&spec, &name] {   // 내가 만든 스레드는 그 스레드가 자기에게 건다
    cyra::apply_checked(spec.stage(name, "infer_worker"));
    cyra::verify(spec.stage(name, "infer_worker"), cyra::this_tid());
  });
  worker.join();
}
for (const cyra::Stage * e : spec.external()) {   // cyra가 안 거는 단계
  log(e->node, cyra::verify(*e, foreign_tid).to_string());   // 기록만 하지 않고 확인한다
}
```

`cyra::Stage`는 파일이 그 단계에 대해 아는 전부를 담는다. `node`·`group`·`external`·`expect_priority`·`opts`·`strict`·`control_priority`, 그리고 어느 체인이 선언했는지(`chain`)다. `cyra::Spec`은 `stages()`로 전부, `find()`로 하나, `external()`로 cyra가 안 거는 것만 준다. Python도 같은 파서로 파일을 읽는다(4절).

- `stages`는 데이터가 흐르는 순서다. RT 우선순위가 그 방향으로 커져야 한다. 상류가 하류를 선점하면 굶긴다. `policy: other`처럼 RT가 아닌 단계는 이 규칙 밖이다.
- `control_priority`는 파일이 유도한다. 체인의 마지막 RT 우선순위가 control이고, 그 단계 자신은 0이다.
- 모르는 키는 거절한다. 버전 필드는 없다.
- 한 단계가 체인 둘에 나오는 것은 정상이다. 두 체인이 서로 다르게 선언하면 거절한다. `strict`는 그 단계가 속한 체인 중 가장 엄격한 것이다. hard 체인에 속한 단계는 파일에서 어느 체인이 먼저 나오든 hard다.
- hard 체인은 RT 단계마다 코어를 따로 준다. 둘이 같은 코어에 핀되면 거절한다. `SCHED_FIFO`에는 타임슬라이스가 없어 하나가 다른 하나를 완전히 막는다. soft 체인은 허용한다.
- `external` 단계는 `expect_priority`만 적는다. 1..99이고 0은 미선언이다. `policy`/`priority`/`cpus`는 못 가지고, 단계를 거는 진입점은 모두 거절한다.
- `CYRA_SPEC`이 없거나 비면 빈 spec이다. 아무것도 선언 안 한 것과 같은 no-op이다.
- `stage(node, group)`는 노드의 전체 이름(`get_fully_qualified_name()`)을 받는다. 파일에 없는 라벨을 찾으면 던진다.
- `apply_checked(stage)`는 호출한 스레드를 그 단계에 건다. 체인의 strictness와 control 우선순위가 따라온다. stage를 통째로 받는 이유는 `strict`와 `control_priority`를 필드로 풀면 흘리기 때문이다. 한 단계는 한 스레드다. 이 프로세스의 살아 있는 스레드 하나가 단계를 쥐고 있는 동안 다른 스레드가 같은 단계를 걸면 `std::invalid_argument`를 던진다. 쥔 스레드가 다시 거는 것은 된다. 스레드가 끝나면 풀린다.
- `verify(stage, tid)`는 아무것도 안 걸고 선언과 실제만 대조한다. finding은 `observed-policy`·`observed-priority`·`observed-cpus` 셋이고, 파일이 주장하지 않는 항목은 `Unknown`이다. `observed-cpus`는 스레드가 선언한 cpu 안에서 돌면 Ok다. `apply_checked`가 되읽는 기준과 같아서, cpuset이 마스크를 좁힌 것은 통과하고 선언 밖 cpu는 Fail이다. 읽을 수 없는 스레드는 `observed-thread` Fail이다. `tid`는 호출자가 댄다.
- `verify_process(spec)`는 반대 질문을 한다. 이 프로세스에 파일이 설명하지 못하는 RT 스레드가 있는가. 어떤 단계가 선언한 policy·priority·cpus로 도는 스레드는 그 단계가 설명한다. 콜백이 만들고 기다리는 자식도 그 값을 그대로 물려받으므로 따로 선언할 필요가 없다. 아무 단계도 설명하지 못하는 스레드마다 tid를 적은 `undeclared-rt-thread` Warn이 하나씩 나오고, 전부 설명되면 `undeclared-rt-thread` Ok가 하나 나온다. Fail이 아니라 Warn인 이유는, 선언하지 않은 스레드가 잘못인지 cyra가 알 수 없기 때문이다.

## 3. flux PartitionedExecutor (C++, `cyra_flux`)

`flux::ros::PartitionedExecutor`는 콜백 그룹마다 스레드를 따로 준다. `cyra::flux::schedule`이 그 스레드에 선언을 건다.

```cpp
#include "cyra/flux.hpp"

flux::ros::PartitionedExecutor ex;
ex.add_ros_node(node);
cyra::flux::schedule(ex, group, {cyra::Policy::Fifo, 90, {3}});   // 값으로
cyra::flux::schedule(ex, *node, infer_group, spec.stage(name, "infer"));   // 파일의 단계로
ex.spin();
```

- 그룹을 맡은 자식 스레드가 첫 콜백 전에 `apply_checked`로 자기에게 건다. 거절은 `spin()`의 에러로 올라온다. 약한 설정으로 내려가지 않는다.
- `schedule(ex, group, opts, strict, control_priority)`는 `opts`를 그 자리에서 검사한다. `strict`의 기본은 `Soft`, `control_priority`의 기본은 0(미선언)이다.
- `schedule(ex, node, group, stage)`는 단계의 이름과 그룹을 그 자리에서 대조한다. rclcpp 콜백 그룹에는 이름이 없어서, 이 짝이 파일의 `(node, label)`과 스레드를 잇는 유일한 연결이다. 그룹은 `node`의 것이어야 하고, `node`는 단계가 가리키는 노드여야 하며, 라벨이 비면 그 노드의 기본 콜백 그룹이어야 한다. `external` 단계는 거절한다.
- 그룹당 선언 하나이고, 그 그룹은 `ex`가 맡아야 한다. 둘 다 executor가 검사한다. 두 번째 선언은 그 자리에서 던지고, 어느 자식도 맡지 않는 그룹의 선언은 `spin()`이 거절한다.
- 한 단계를 두 그룹에 주면 `spin()`이 거절한다. 한 단계는 한 스레드이므로(2절) 두 번째로 거는 자식이 던진다.
- `spin()`은 `SCHED_OTHER` 스레드에서 부른다. 자식은 호출 스레드의 스케줄링으로 시작하므로, RT 호출자에서는 선언이 없는 그룹이 그 우선순위로 선언된 그룹들보다 위에서 돈다. flux는 이것을 검사하지 않는다. `verify_process`가 그런 스레드를 `undeclared-rt-thread`로 보고한다.
- message_filters synchronizer 하나의 입력은 전부 한 그룹에 둔다. 동기화 정책은 짝이 맞은 콜백을 자기 `std::mutex`를 쥔 채로 돌리고 그 뮤텍스에는 priority inheritance가 없어, 입력이 두 그룹 스레드에 걸치면 두 그룹의 우선순위가 묶인다.
- message_filters 경로는 할당이 있다(정책 큐, 프레임마다 `shared_ptr` control block). hard 체인에는 놓지 않는다. 필터 없는 flux 배달에는 할당이 없다.

## 4. Python (`cyra_py`)

`cyra`는 C++ 코어를 그대로 묶은 것이다. 적용하는 쪽만 나와 있다. `preflight`·`Report`·`Strictness`는 hard RT 판정 장치라 Python 표면에 없다.

RT 보장이 아니다. 어떤 우선순위를 걸어도 GIL과 GC는 상한 없는 지연원으로 남는다. 정하는 것은 여러 스레드가 동시에 runnable일 때 커널이 누구를 고르는지와 각자 어느 코어를 쓰는지다. 그래도 정할 값이 있는 이유는 GIL을 놓는 구간에서 스레드가 실제로 병렬로 돌기 때문이다. numpy·zlib·디코딩이 그 구간이다.

```python
import cyra

report = cyra.apply(cpus=[4])          # 부른 스레드 자신
state = cyra.current()                 # 커널에서 되읽기
klass, prio, where = state.policy, state.priority, state.cpus
tid = cyra.this_tid()                  # top -H와 /proc이 부르는 번호

spec = cyra.Spec.load()                # CYRA_SPEC. 없으면 빈 spec
if not spec.empty():
    cyra.apply(spec.stage(node.get_fully_qualified_name(), "infer_worker"))
```

- `cyra.apply(policy=, priority=, cpus=)`는 부른 스레드에 걸고 preflight를 `Soft`로 돌린다. 건 뒤 커널에서 되읽어 대조하므로, cpuset이 마스크를 좁혀 요청이 그대로 안 앉은 경우가 성공으로 안 지나간다. 커널 거절은 `OSError`, 요청이 이 호스트에서 성립 안 하거나 되읽기가 어긋나면 `RuntimeError`, 값 범위가 틀리면 `ValueError`다. 아무것도 요청 안 하면 아무것도 안 한다.
- `cyra.apply(stage)`는 부른 스레드를 그 단계에 건다. hard 체인의 단계는 `Hard`로 걸리므로 preflight의 Warn에서도 거절된다. external 단계나 살아 있는 다른 스레드가 쥔 단계는 `ValueError`다.
- `apply`가 돌려주는 문자열은 호스트 판정 보고다. 사람이 읽으라고 있는 것이고 파싱할 형식이 아니다. 요청이 없으면 빈 문자열이다.
- `policy`는 `cyra.Policy`의 `Inherit`(기본, 안 건드림)·`Other`(SCHED_OTHER)·`Fifo`(SCHED_FIFO)·`RoundRobin`(SCHED_RR)이다. `Fifo`·`RoundRobin`은 `priority` 1..99와 `RLIMIT_RTPRIO` 또는 `CAP_SYS_NICE`가 필요하다. affinity만 걸 때는 권한이 필요 없다.
- `cyra.current()`는 `cyra.ThreadState`를 준다. `policy`는 `Policy`가 아니라 raw `SCHED_*` 정수다(`os.SCHED_FIFO`와 비교한다). `priority`는 정수, `cpus`는 코어 번호 목록이다.
- `cyra.this_tid()`는 커널 thread id다. `threading.get_ident()`가 주는 번호와 다르다.
- `cyra.Spec.load(path=None)`는 `path`를, None이면 `CYRA_SPEC`이 가리키는 파일을 읽는다. 변수가 없으면 빈 spec이고 `empty()`가 그것을 알려 준다. 파일이 규칙을 어기면 `RuntimeError`다. `spec.stage(node, group="")`는 노드의 전체 이름을 받고, 파일에 없는 단계면 `RuntimeError`다.
- `cyra.Stage`는 읽기 전용이다: `node`·`group`·`chain`·`external`·`expect_priority`·`policy`·`priority`·`cpus`·`control_priority`.

`cyra.flux`는 `flux.ros.PartitionedExecutor` 스레드의 스케줄링을 선언한다.

```python
import cyra.flux

cyra.flux.set_thread_scheduling(ex, group, cpus=[4, 5])
cyra.flux.set_thread_scheduling(ex, node, policy=cyra.Policy.Fifo, priority=20)
cyra.flux.schedule(ex, infer_group, spec.stage(name, "infer"))
```

- `unit`은 `add`에 준 콜백 그룹이거나 `add_ros_node`에 준 노드다. C++은 콜백 그룹 하나지만 rclpy에서는 단위가 둘로 갈린다.
- 자식이 첫 콜백 전에 자기에게 걸고, 거절은 `spin()`의 예외가 된다. external 단계도 거기서 거절된다.
- 두 함수를 합쳐 unit당 선언 하나다. executor가 안 돌리는 unit의 선언은 `spin()`이 거절한다.
- 단계의 노드를 unit과 대조하지 않는다. C++은 대조하지만, Python의 flux 그룹은 자기 노드를 모른다.
- `flux.ros.Executor`는 부른 쪽 스레드에서 돌므로 그 `spin()` 전에 `cyra.apply()`를 직접 부른다.

## 5. flux와의 연결

`cyra_flux`와 `cyra.flux`는 flux를 직접 링크하고 executor의 `on_thread_start` hook을 등록한다(C++ `PartitionedExecutor::on_thread_start(group, fn)`, Python `PartitionedExecutor.on_thread_start(unit, fn)`). flux는 cyra를 모른다. `cyra_core`와 `cyra`는 flux에 의존하지 않는다.

이 연결 방식은 고정이 아니다. hook, 직접 링크, 패키지 구성은 언제든 바뀔 수 있으며, `cyra_flux`와 `cyra.flux` 밖의 코드는 여기에 기대지 않는다.
