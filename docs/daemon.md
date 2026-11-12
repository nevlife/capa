# 데몬 `capad`

라이브러리는 스레드 안에서 그 스레드에 적용한다. `bind`, `spawn`, flux 어댑터는 모두 적용 대상 스레드 위에서 실행된다. 그래서 사용자 코드가 실행되지 않는 스레드(인터럽트 스레드, DDS와 SDK의 스레드)에는 라이브러리로 적용 불가능하다. `capad`(패키지 `capa_daemon`)는 이런 스레드에 밖에서 적용한다. 대상은 `match`를 적은 stage뿐이고, capa의 나머지 기능은 데몬 없이 동작한다.

```text
capad [--spec FILE] [--interval SEC] [--once]
```

- 파일은 `--spec`으로 주고, 없으면 `CAPA_SPEC`을 사용한다.
- capad는 `--interval`(기본 1 s)마다 스레드를 확인한다. 그 사이의 커널 프로세스 이벤트(proc connector)로 `match` stage의 결과가 바뀔 가능성을 판단하고, 그런 경우와 30 s마다 `/proc`을 전부 읽는다. 나머지 경우에는 이미 찾은 스레드의 드리프트만 확인한다.
- 이벤트를 받지 못하는 커널이나 권한에서는 시작 시 `no process events`를 출력하고 매번 `/proc`을 전부 읽는다.
- `--once`를 주면 한 번만 확인하고 종료한다. 적용한 설정은 되돌리지 않는다.

| 시그널 | 동작 |
| --- | --- |
| SIGHUP | 파일을 다시 읽는다(아래) |
| SIGTERM, SIGINT | 적용한 스레드와 이동한 자식을 전부 원래 값으로 되돌리고 종료한다 |

SIGHUP 시 동작:

- 규칙을 어기는 파일은 거절하고 이전 파일을 유지한다.
- 새 파일에서도 같은 방식으로 match하는 stage의 스레드는 그대로 두고, 나머지 스레드는 원래 값으로 되돌린다.
- 이동한 자식은 stage의 새 `children_cpus`로 이동한다. stage에서 `children_cpus`가 빠졌으면 원래 CPU로 되돌린다.

## 상태 파일

capad는 적용하거나 이동한 스레드를 각 스레드의 원래 값과 함께 `CAPA_DAEMON_DIR`(기본 `/run/capa`)의 `capad.state`에 기록한다.

- 무엇이 바뀔 때마다 파일 전체를 다시 쓴다.
- 형식은 `capad-state 2`이고, `arg`가 없던 `capad-state 1` 파일도 읽는다.
- capad가 정상 종료하지 못하고 죽으면(`kill -9`, 크래시) 파일이 남는다. 다음 capad는 첫 pass 전에 그 파일을 이어받는다. 방식은 SIGHUP과 같다. 여전히 같은 방식으로 match하는 stage의 스레드는 처음 값과 함께 맡고, 나머지는 즉시 되돌린다. 그래서 재시작한 capad가 멈출 때도 스레드는 처음 값으로 복원된다.
- `/run`은 부팅 시 비워지고, 스레드도 그때 새로 시작한다.

## 한 번 확인할 때

| stage | capad의 동작 |
| --- | --- |
| `match` | 아래 "`match` stage" |
| 스레드가 bind한 stage(`bind`, `spawn`, flux) | 드리프트는 보고만 하고 바꾸지 않는다. 그 stage는 bind한 스레드가 맡는다. 예외는 아래 "권한 위임" |
| `external` | 아무것도 하지 않는다 |

`match` stage:

1. match가 가리키는 스레드를 찾는다.
2. 정확히 하나면 그 스레드에 대해 `preflight`를 실행한다. 메모리 잠금은 그 스레드의 프로세스로 판정한다.
3. 판정이 거절(`Fail`, 또는 hard 체인의 `Warn`)이면 적용하지 않는다. 아니면 적용하고, `apply_checked`처럼 되읽어서 선언과 다르면 되돌린다.
4. 찾은 스레드가 없거나 둘 이상이거나 판정이 거절이면, 전에 적용한 스레드를 처음 값으로 되돌리고 아무것도 적용하지 않는다.
5. 찾은 스레드가 전과 다르면 이전 스레드를 먼저 되돌리고 새 스레드에 적용한다.

권한 위임: 권한이 없어 capad에 요청한 스레드에는 capad가 그 스레드에 대해 `preflight`를 실행한 뒤 체인 파일의 값을 적용한다. 이 스레드는 되돌릴 대상으로 기억하지 않는다. 같은 stage를 요청한 스레드가 둘 이상이면 아무것도 적용하지 않는다. `children_cpus`가 있으면 그 stage의 reset 자식을 그 CPU로 이동한다.

- capad는 되돌리기 가능한 스레드에만 적용한다. 대상은 SCHED_OTHER, SCHED_FIFO, SCHED_RR 스레드다. SCHED_BATCH, SCHED_IDLE, SCHED_DEADLINE 스레드는 보고하고 그대로 둔다.
- 커널은 tid를 재사용하므로, capad는 시작 시각으로 같은 스레드인지 구별한다. 적용이나 이동 도중에 시작 시각이 바뀌면 그 사실을 보고하고 그 스레드를 맡지 않는다.

### reset 자식

커널은 어느 스레드가 어느 스레드를 생성했는지 알려 주지 않는다. 그래서 capad는 부모에게서 상속받은 값으로 reset 자식을 식별한다. stage를 점유한 프로세스의 스레드 중 아래 조건을 모두 만족하는 스레드가 reset 자식이다.

- stage를 점유하지 않았다.
- stage를 bind할 때 없었다(bind가 그때 있던 스레드를 기록한다).
- SCHED_OTHER로 실행된다.
- 허용 CPU가 stage의 cpus와 정확히 같다.

bind 뒤에 다른 스레드가 생성한 스레드라도 스스로 정확히 그 CPU에 고정했다면 reset 자식과 구별되지 않아 함께 이동된다. capad는 이동한 스레드를 원래 CPU와 함께 기억한다.

### 기록

capad도 자기가 적용한 stage를 기록하므로, `capa status`에 capad가 적용한 스레드의 pid와 tid가 나온다.

- 이 기록은 다른 프로세스의 스레드를 가리키므로 bind 기록과 다른 디렉터리인 `CAPA_DAEMON_DIR`(기본 `/run/capa`)에 저장한다.
- 디렉터리가 없으면 생성한다. 디렉터리가 자기 사용자의 것이 아니거나 다른 사용자가 쓰기 가능하면 기록하지 않고, 읽는 쪽도 그런 디렉터리를 무시한다.
- 바뀐 것이 있을 때마다 stderr에 한 줄씩 출력한다.

## 찾는 방법

| `match` | 찾는 것 |
| --- | --- |
| `{irq: 3550000.usb}` | 그 핸들러의 인터럽트 스레드 |
| `{process: cam, thread: dds.recv}` | comm이 `cam`인 프로세스 안에서 comm이 `dds.recv`인 스레드 |
| `{arg: "__node:=planning_container", thread: recvUC}` | `/proc/<pid>/cmdline`의 원소 하나가 정확히 `__node:=planning_container`인 프로세스 안에서 comm이 `recvUC`인 스레드 |

- irq: 그 이름을 가진 `/sys/kernel/irq/<n>/actions`마다 `irq/<n>-3550000.usb` 스레드를 찾는다. 5.17 이전 커널은 이름을 15 B로 자른다. `irq/<n>-s-...` 보조 스레드는 제외한다.
- comm은 15 B라서 긴 이름은 앞 15 B로 비교한다.
- arg: cmdline을 NUL로 나눈 원소와 비교한다. 같은 실행 파일로 시작한 컨테이너 여러 개처럼 comm이 같은 프로세스들을 구별할 때 사용한다. `process`도 적으면 둘 다 맞아야 한다.
- match는 패턴이 아니라 정확한 이름이다. `arg`도 원소 전체를 비교하고, 그 원소를 포함하는 더 긴 원소는 맞지 않는다.
- match 값에는 탭과 줄바꿈을 사용하지 않는다. 상태 파일이 줄 단위이고 필드를 탭으로 나누기 때문이다.
- 스레드를 생성하는 라이브러리가 허용하면 `pthread_setname_np`로 스레드 이름을 지정한다.

## 빌드

capad는 Rust로 작성했다. 설정 파일 읽기, 규칙, 판정은 모두 `capa_core`에 있고, capad는 `cxx` bridge(`src/ffi.rs`, `src/bridge.cpp`)로 그것을 호출한다. 그래서 파서는 하나다. colcon은 capad를 다른 패키지처럼 빌드한다. CMake가 `capa_core`를 찾아 그 라이브러리 목록을 cargo에 넘기고, 생성된 `capad`를 설치한다. `colcon test`는 `cargo test`를 실행한다.

- rustup의 toolchain이 필요하다. `rust-toolchain.toml`이 버전을 1.98.1로 고정한다. 배포판 cargo(Ubuntu 24.04 apt의 1.75)는 이 파일을 무시하고 2024 edition을 빌드하지 못한다. CMake는 `PATH`보다 `$CARGO_HOME/bin`(기본 `~/.cargo/bin`)에서 cargo를 먼저 찾는다.

  ```text
  curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- --default-toolchain 1.98.1
  ```

- 첫 빌드는 `Cargo.lock`이 고정한 crate를 내려받으므로 한 번은 네트워크가 필요하다.

## 권한

다른 사용자의 스레드에 적용하려면 `CAP_SYS_NICE`가 필요하다. `systemd/capad.service`는 capad를 전용 사용자로 실행하고 그 권한 하나만 준다. 권한이 없으면 적용이 "setting a thread of another user needs CAP_SYS_NICE"로 실패하고, capad는 이를 보고하고 계속 실행된다.

match로 지정한 스레드는 capad가 찾기 전까지 설정 없이 실행된다. 인터럽트 스레드는 부팅 시부터 있으므로, capad를 앱보다 먼저 시작하면 설정 없이 실행되는 구간이 생기지 않는다. 코드 삽입이 가능한 스레드는 스스로 `bind`하는 편이 낫다. 그러면 스레드가 일을 시작하기 전에 설정이 적용된다.
