# 데몬 `capad`

라이브러리는 스레드 안에서 그 스레드를 건다. `bind`, `spawn`, flux 어댑터는 모두 자기가 거는 스레드 위에서 돈다. 그래서 우리 코드가 돌지 않는 스레드는 라이브러리로 걸 수 없다. 인터럽트 스레드, DDS와 SDK의 스레드가 그렇다. `capad`(패키지 `capa_daemon`)는 이런 스레드를 밖에서 건다. 대상은 `match`를 적은 stage뿐이다. capa의 나머지 기능은 데몬 없이 동작한다.

```text
capad [--spec FILE] [--interval SEC] [--once]
```

파일은 `--spec`으로 주고, 없으면 `CAPA_SPEC`을 쓴다. capad는 `--interval`초(기본 1)마다 한 번씩 스레드를 살핀다. 그 사이의 커널 프로세스 이벤트(proc connector)로 `match` stage의 결과가 바뀔 수 있는지 판단해, 그럴 때와 30초마다 `/proc`을 훑는다. 나머지 때는 이미 잡은 스레드의 드리프트만 본다. 이벤트를 받지 못하는 커널이나 권한에서는 시작할 때 `no process events`를 찍고 매번 훑는다. `--once`를 주면 한 번만 훑고 끝나며, 건 설정은 되돌리지 않고 그대로 둔다.

| 시그널 | 동작 |
| --- | --- |
| SIGHUP | 파일을 다시 읽는다. 규칙을 어기는 파일은 거절하고 이전 파일을 유지한다. 새 파일에서도 같은 방식으로 match하는 stage의 스레드는 그대로 두고, 나머지 스레드는 원래 값으로 되돌린다. 옮긴 자식은 stage의 새 `children_cpus`로 옮기고, stage에서 `children_cpus`가 빠졌으면 원래 CPU로 되돌린다 |
| SIGTERM, SIGINT | 건 스레드와 옮긴 자식을 전부 원래 값으로 되돌리고 끝난다 |

capad는 자기가 건 스레드와 옮긴 스레드를 각 스레드의 원래 값과 함께 `CAPA_DAEMON_DIR`(기본 `/run/capa`)의 `capad.state`에 적는다. 무엇이 바뀔 때마다 파일을 통째로 다시 쓴다. 형식은 `capad-state 2`이고, `arg`가 없던 `capad-state 1` 파일도 읽는다. capad가 정상 종료하지 못하고 죽으면(`kill -9`, 크래시) 파일이 남는다. 다음 capad는 첫 pass 전에 그 파일을 이어받는다. 이어받는 방식은 SIGHUP으로 파일을 다시 읽을 때와 같다. 여전히 같은 방식으로 match하는 stage의 스레드는 처음 값과 함께 맡고, 나머지는 바로 되돌린다. 그래서 재시작한 capad가 멈출 때도 스레드는 처음 값으로 돌아간다. `/run`은 부팅 때 비워지고, 스레드도 그때 새로 시작한다.

## 한 번 훑을 때

| stage | capad가 하는 일 |
| --- | --- |
| `match` | match가 가리키는 스레드를 찾는다. 정확히 하나면 그 스레드에 대해 `preflight`를 돌린다(메모리 잠금은 그 스레드의 프로세스로 판정한다). 판정이 거절이면(`Fail`, 또는 hard 체인의 `Warn`) 걸지 않는다. 거절이 아니면 걸고, `apply_checked`처럼 되읽어서 선언과 다르면 되돌린다. 찾은 스레드가 없거나 둘 이상이거나 판정이 거절이면, 전에 건 스레드를 처음 값으로 되돌리고 아무것도 걸지 않는다. 찾은 스레드가 전과 다르면 이전 스레드를 먼저 되돌리고 새 스레드에 건다 |
| 스레드가 bind한 stage(`bind`, `spawn`, flux) | 선언과 달라진 점(드리프트)은 보고만 하고 바꾸지 않는다. 그 stage는 bind한 스레드가 맡기 때문이다. 권한이 없어 capad에 요청한 스레드는 예외다. capad는 `preflight`를 그 스레드에 돌린 뒤 체인 파일의 값을 걸고, 그 스레드는 되돌릴 대상으로 기억하지 않는다. 같은 stage를 요청한 스레드가 둘 이상이면 아무것도 걸지 않는다. `children_cpus`가 있으면 그 stage의 reset 자식을 그리로 옮긴다 |
| `external` | 아무것도 하지 않는다 |

- capad는 되돌릴 수 있는 스레드에만 건다. 대상은 SCHED_OTHER, SCHED_FIFO, SCHED_RR로 도는 스레드다. SCHED_BATCH, SCHED_IDLE, SCHED_DEADLINE 스레드는 보고하고 그대로 둔다.
- 커널은 tid를 재사용하므로, capad는 시작 시각으로 같은 스레드인지 가린다. 걸거나 옮기는 도중에 시작 시각이 바뀌면 그 사실을 보고하고 그 스레드를 맡지 않는다.

커널은 어느 스레드가 어느 스레드를 만들었는지 알려 주지 않는다. 그래서 capad는 reset 자식을 부모에게서 물려받은 값으로 알아본다. reset 자식으로 보는 것은 stage를 쥔 프로세스의 스레드 중 아래 조건을 모두 만족하는 스레드다.

- stage를 쥐지 않았다.
- stage를 bind할 때 아직 없었다(bind가 그때 있던 스레드를 기록한다).
- SCHED_OTHER로 돈다.
- 허용 CPU가 stage의 cpus와 정확히 같다.

bind 뒤에 다른 스레드가 만든 스레드라도 스스로 정확히 그 CPU에 고정했다면, reset 자식과 똑같이 보여서 함께 옮겨진다. capad는 옮긴 스레드를 원래 CPU와 함께 기억한다.

capad도 자기가 건 stage를 기록한다. 그래서 `capa status`에 capad가 건 스레드의 pid와 tid가 나온다. 이 기록은 다른 프로세스의 스레드를 가리키므로, bind 기록과 다른 디렉터리인 `CAPA_DAEMON_DIR`(기본 `/run/capa`)에 둔다. capad는 디렉터리가 없으면 만든다. 디렉터리가 자기 사용자의 것이 아니거나 다른 사용자가 쓸 수 있으면 거기에 쓰지 않는다. 읽는 쪽도 그런 디렉터리는 무시한다. capad는 바뀐 것이 있을 때마다 stderr에 한 줄씩 쓴다.

## 찾는 방법

| `match` | 찾는 것 |
| --- | --- |
| `{irq: 3550000.usb}` | 그 핸들러의 인터럽트 스레드. 그 이름을 가진 `/sys/kernel/irq/<n>/actions`마다 `irq/<n>-3550000.usb` 스레드(5.17 이전 커널은 15바이트로 자른 이름). `irq/<n>-s-...` 보조 스레드는 제외 |
| `{process: cam, thread: dds.recv}` | comm이 `cam`인 프로세스 안에서 comm이 `dds.recv`인 스레드. comm은 15바이트라 긴 이름은 앞 15바이트로 비교한다 |
| `{arg: "__node:=planning_container", thread: recvUC}` | `/proc/<pid>/cmdline`을 NUL로 나눈 원소 하나가 정확히 `__node:=planning_container`인 프로세스 안에서 comm이 `recvUC`인 스레드. 같은 실행 파일로 띄운 컨테이너 여러 개처럼 comm이 같은 프로세스들을 구별할 때 쓴다. `process`도 적으면 둘 다 맞아야 한다 |

match는 패턴이 아니라 정확한 이름이다. `arg`도 원소 전체를 비교하고, 그 원소를 포함하는 더 긴 원소는 맞지 않는다. match 값에는 탭과 줄바꿈을 쓰지 않는다. 상태 파일이 줄 단위이고 필드를 탭으로 나누기 때문이다. 스레드를 만드는 라이브러리가 허용하면 `pthread_setname_np`로 스레드에 이름을 붙인다.

## 빌드

capad는 Rust로 썼다. 설정 파일 읽기, 규칙, 판정은 모두 `capa_core`에 있고, capad는 `cxx` bridge(`src/ffi.rs`, `src/bridge.cpp`)로 그것을 부른다. 그래서 파서는 여전히 하나다. colcon은 capad를 다른 패키지처럼 빌드한다. CMake가 `capa_core`를 찾아 그 라이브러리 목록을 cargo에 넘기고, 만들어진 `capad`를 설치한다. `colcon test`는 `cargo test`를 돌린다.

- rustup의 toolchain이 필요하다. `rust-toolchain.toml`이 버전을 1.98.1로 고정한다. 배포판 cargo(Ubuntu 24.04 apt의 1.75)는 이 파일을 무시하고 2024 edition을 빌드하지 못한다. CMake는 `PATH`보다 `$CARGO_HOME/bin`(기본 `~/.cargo/bin`)에서 cargo를 먼저 찾는다.

  ```text
  curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- --default-toolchain 1.98.1
  ```

- 첫 빌드는 `Cargo.lock`이 고정한 crate를 내려받는다. 그래서 한 번은 네트워크가 필요하다.

## 권한

다른 사용자의 스레드를 걸려면 `CAP_SYS_NICE`가 필요하다. `systemd/capad.service`는 capad를 전용 사용자로 돌리고, 그 권한 하나만 준다. 권한이 없으면 걸기가 "setting a thread of another user needs CAP_SYS_NICE"로 실패한다. capad는 이를 보고하고 계속 돈다.

match로 지목한 스레드는 capad가 찾을 때까지 설정 없이 돈다. 인터럽트 스레드는 부팅 때부터 있으므로, capad를 앱보다 먼저 띄우면 설정 없이 도는 구간이 생기지 않는다. 코드를 넣을 수 있는 스레드는 스스로 `bind`하는 편이 낫다. 그러면 스레드가 일을 시작하기 전에 설정이 걸린다.
