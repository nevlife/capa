# 데몬 `cyrad`

라이브러리는 스레드 안에서 건다. `bind`, `spawn`, flux 어댑터는 자기가 거는 스레드 위에서 돈다. 그러면 우리 코드가 돌지 않는 스레드가 남는다. 인터럽트 스레드, DDS와 SDK 스레드다. `cyrad`(패키지 `cyra_daemon`)가 `match`를 적은 stage에 한해 이 스레드들을 밖에서 건다. 나머지 기능은 데몬 없이 동작한다.

```text
cyrad [--spec FILE] [--interval SEC] [--once]
```

파일은 `--spec`, 없으면 `CYRA_SPEC`이다. `--interval`초(기본 1)마다 한 번씩 훑는다. `--once`는 한 번만 훑고 끝나며, 건 것은 그대로 둔다.

| 시그널 | 동작 |
| --- | --- |
| SIGHUP | 파일을 다시 읽는다. 규칙을 어기는 파일은 거절하고 이전 파일을 유지한다. 같은 방식으로 여전히 match하는 stage의 스레드는 그대로 두고, 나머지는 되돌린다 |
| SIGTERM, SIGINT | 건 스레드를 전부 원래 값으로 되돌리고 끝난다 |

## 한 번 훑을 때

| stage | cyrad가 하는 일 |
| --- | --- |
| `match` | stage의 `preflight`를 돌리고, 거절이면(`Fail`, 또는 hard 체인의 `Warn`) 건너뛴다. match가 가리키는 스레드를 찾는다. 정확히 하나면 걸고 `apply_checked`처럼 되읽으며, 벗어나면 되돌린다. 없으면 match 안 됨으로 보고한다. 둘 이상이면 추측하지 않고 아무것도 걸지 않는다 |
| 스레드가 bind한 stage(`bind`, `spawn`, flux) | 드리프트를 보고만 하고 바꾸지 않는다. stage의 주인은 그 스레드다. `children_cpus`가 있으면 그 stage의 reset 자식을 그리로 옮긴다 |
| `external` | 아무것도 안 한다 |

reset 자식은 물려받은 흔적으로 알아본다. stage를 쥔 프로세스의 스레드 중 stage를 쥐지 않았고, SCHED_OTHER이고, 허용 CPU가 stage의 cpus와 정확히 같은 스레드다.

cyrad도 bind하는 프로세스처럼 자기가 건 stage를 기록한다. 그래서 `cyra status`에 cyrad가 건 스레드의 pid와 tid가 나온다. 바뀐 것이 있을 때마다 stderr에 한 줄씩 쓴다.

## 찾는 방법

| `match` | 찾는 것 |
| --- | --- |
| `{irq: 3550000.usb}` | 그 핸들러의 인터럽트 스레드. 그 이름을 가진 `/sys/kernel/irq/<n>/actions`마다 `irq/<n>-3550000.usb` 스레드(5.17 이전 커널은 15바이트로 자른 이름). `irq/<n>-s-...` 보조 스레드는 아니다 |
| `{process: cam, thread: dds.recv}` | comm이 `cam`인 프로세스 안에서 comm이 `dds.recv`인 스레드. comm은 15바이트라 긴 이름은 앞 15바이트로 비교한다 |

match는 패턴이 아니라 정확한 이름이다. 라이브러리가 허용하면 `pthread_setname_np`로 스레드 이름을 붙인다.

## 빌드

cyrad는 Rust다. 설정 파일, 규칙, 판정은 모두 `cyra_core`에 있고, cyrad는 `cxx` 다리(`src/ffi.rs`, `src/bridge.cpp`)로 그것을 부른다. 그래서 파서는 여전히 하나다. colcon은 다른 패키지처럼 빌드한다. CMake가 `cyra_core`를 찾아 그 라이브러리 목록을 cargo에 넘기고, 만들어진 `cyrad`를 설치한다. `colcon test`는 `cargo test`를 돌린다.

- rustup의 toolchain이 필요하다. `rust-toolchain.toml`이 1.98.1로 고정한다. 배포판 cargo(Ubuntu 24.04 apt의 1.75)는 이 파일을 무시하고 2024 edition을 빌드하지 못한다. CMake는 `PATH`보다 `$CARGO_HOME/bin`(기본 `~/.cargo/bin`)을 먼저 본다.

  ```text
  curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- --default-toolchain 1.98.1
  ```

- 첫 빌드는 `Cargo.lock`이 고정한 crate를 내려받는다. 그래서 한 번은 네트워크가 필요하다.

## 권한

다른 사용자의 스레드를 걸려면 `CAP_SYS_NICE`가 필요하다. `systemd/cyrad.service`는 전용 사용자로, 그 권한 하나만 주고 돌린다. 권한이 없으면 "setting a thread of another user needs CAP_SYS_NICE"로 실패하고, cyrad는 그것을 보고하고 계속 돈다.

match한 스레드는 cyrad가 찾을 때까지 설정 없이 돈다. 인터럽트 스레드는 부팅 때부터 있으므로, cyrad를 앱보다 먼저 띄우면 그 구간이 생기지 않는다. 코드를 넣을 수 있는 스레드는 스스로 `bind`하는 게 낫다. 그러면 일을 시작하기 전에 걸린다.
