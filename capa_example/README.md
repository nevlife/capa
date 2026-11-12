# capa_example

capa로 스레드 스케줄링을 적용하는 C++ 예제다. Python은 GIL과 GC 때문에 지연에 상한이 없어서 RT 대상이 아니다.

```bash
ros2 run capa_example preflight

export CAPA_SPEC=$(ros2 pkg prefix capa_example)/share/capa_example/config/perception_to_control.yaml
ros2 run capa_example chain
```

## preflight, 적용 전 판정

커널은 deadline 준수가 불가능한 호스트에서도 스케줄 요청을 받아들인다. `preflight()`는 아무것도 바꾸기 전에 이 호스트가 스케줄을 감당 가능한지 판정한다. 판정 근거는 커널이 보고하는 값(rlimit, `/proc`, `/sys`)이다.

```cpp
capa::Options opts;
opts.policy = capa::Policy::Fifo;
opts.priority = 80;
opts.cpus = {2};
const capa::Result<capa::Report> report = capa::preflight(opts, 90);
```

- `control_priority`가 없으면 `priority-order` 검사는 `Unknown`이다. 실행되지 않은 검사가 통과로 보이지 않게 하기 위해서다.

| 판정 | 뜻 | soft | hard |
| --- | --- | --- | --- |
| `Fail` | 요청을 적힌 대로 적용 불가능하다 | 거절 | 거절 |
| `Warn` | 적용은 가능하지만 호스트가 지연에 상한을 두도록 설정되지 않았다 | 보고만 한다 | 거절 |

## chain, 스레드당 한 줄

체인은 여러 프로세스에 걸쳐 있어서, 어느 노드도 자기 설정이 지켜야 할 순서를 혼자서는 확인 불가능하다. 그래서 스케줄은 파일 하나에 적고, 각 노드는 자기 스레드가 어느 stage인지만 지정한다.

```cpp
capa::Result<void> bound = capa::flux::bind(ex, *node, node->group(), "work");
```

group을 맡은 자식 스레드는 첫 콜백 전에 stage를 자기에게 적용한다. 거절되면 spin이 이유와 함께 종료된다. `CAPA_SPEC`이 없으면 아무것도 등록하지 않는다.

파일은 `config/perception_to_control.yaml`이다.

```yaml
chains:
  perception_to_control:
    target: soft
    stages:
      - node: /flux_loan_image_pub
        external: true
        expect_priority: 0
      - node: /capa_stage
        group: work
        policy: fifo
        priority: 70
        cpus: [2]
      - node: /flux_control
        group: loop
        policy: fifo
        priority: 90
        cpus: [3]
```

- `external` stage는 capa가 적용하지 않는 스레드다. capa는 이 스레드에 `verify()`만 실행한다.
- 파일 규칙(흐름, release, 중요도, hard 코어, 자식)은 `docs/api.md` 2절에 있다.

`chain`을 root가 아닌 사용자로 실행하려면 `RLIMIT_RTPRIO`가 70 이상이어야 한다. 한도가 모자라면 `preflight`가 먼저 알려 준다.
