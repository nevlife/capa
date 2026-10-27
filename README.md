# capa

capa는 데이터가 지나가는 경로에 있는 ROS 2 노드의 스레드를 스케줄링한다. 체인 위의 모든 스레드가 어떻게 도는지는 파일 하나에 선언한다. 선언하는 것은 정책, 우선순위, CPU, 그리고 자식에게 물려줄 것이다.

capa는 이 파일을 통째로 검사한다. 두 stage가 CPU를 공유하면 하류가 상류보다 높아야 한다. release 스레드는 자기가 풀어 주는 작업보다 높아야 한다. 더 중요한 체인이 덜 중요한 체인보다 앞선다.

각 스레드는 코드 한 줄로 자기 stage를 건다. capa는 걸기 전에 호스트 설정을 판정하고, 건 뒤에는 커널에서 값을 다시 읽어 확인한다. 이름은 Chain-Aware Priority and Affinity의 머리글자다.

```cpp
capa::bind("/control_node", "loop");                     // this thread
auto t = capa::spawn("/perception_node", "infer", fn);   // a new thread, set before fn runs
capa::flux::bind(ex, *node, group, "grab");              // a flux PartitionedExecutor group
```

코드를 고칠 수 없는 컴포넌트 노드의 스레드는 컨테이너가 건다.

```bash
CAPA_SPEC=chain.yaml ros2 run capa_ros capa_component_container
```

| 패키지 | 내용 |
| --- | --- |
| `capa_core` | C++17, ROS 없음. 체인 파일(`capa::Spec`), `bind`, `spawn`, `apply_checked`, `preflight`, `verify`, `verify_process` |
| `capa_ros` | ROS 2 콜백 그룹 스레드용. flux `PartitionedExecutor` 그룹에 거는 같은 이름의 함수(`capa/flux.hpp`)와, 파일이 선언한 그룹을 거는 컴포넌트 컨테이너 `capa_component_container` |
| `capa_py` | 같은 이름의 Python(`capa`와 `capa.flux`)과 `capa` 명령. `capa check`와 `capa status`는 데몬 없이 읽기만 한다([`docs/cli.md`](docs/cli.md)) |
| `capa_daemon` | `capad`. Rust로 썼고 cxx bridge로 `capa_core`를 부른다. 파일이 이름으로 지목한 스레드(인터럽트, DDS, SDK 스레드)를 걸고 reset된 자식을 옮긴다([`docs/daemon.md`](docs/daemon.md)) |
| `capa_example` | 예제 `preflight`와 `chain`. C++ 전용 |

API는 [`docs/api.md`](docs/api.md)에 있다. 호스트에서 지연을 재는 방법은 [`docs/latency.md`](docs/latency.md)에 있다.

## 빌드

ROS 2 Humble과 Jazzy를 지원한다.

```bash
git submodule update --init          # thirdparty/nanobind
source /opt/ros/jazzy/setup.bash
cd <workspace>
colcon build --symlink-install
colcon test
```

`capa_core`는 ROS 없이도 빌드된다. `libyaml-cpp-dev`를 설치하고 `cmake -S capa_core -B build`를 한다. `-DCAPA_FUZZER=ON -DCMAKE_CXX_COMPILER=clang++`이면 libFuzzer용 `fuzz_spec`을 빌드한다. 이 옵션이 없으면 ctest가 같은 하니스를 고정된 corpus로 돌린다.

## 검사

검사는 `pre-commit install`로 켠다. 언어마다 도는 검사는 아래와 같다.

| 언어 | 포맷 | 린트 |
| --- | --- | --- |
| C++ | clang-format | clang-tidy. 결함 부류, C++ Core Guidelines, CERT 검사를 켜고, `.clang-tidy`에 이유를 적은 검사만 끈다. 빌드가 만든 `compile_commands.json`이 필요하다 |
| Rust(`capa_daemon`) | rustfmt | clippy `pedantic`, `-D warnings`(bridge의 C++ 쪽도 컴파일한다). `unsafe` 블록에는 연산 하나만 넣고 `SAFETY:` 주석을 단다. 이런 블록은 bridge와 시그널 마스크에만 있다. `Cargo.lock`이 바뀌면 cargo audit로 RustSec advisory DB와 대조한다(`cargo install cargo-audit`와 네트워크가 필요하다) |
| Python | ruff-format | flake8(결함 부류). mypy는 추적하는 stub `capa/_capa.pyi`를 써서 `capa` 전체를 검사한다 |
| Markdown | markdownlint `--fix` | markdownlint |
| Shell | | shellcheck |
| YAML | | yamllint |

트리 전체에 걸친 검사도 있다. 공개 이름이 전부 사용자 문서에 있는지(`scripts/check_api_docs.py`), 계약표가 있는 테스트를 가리키는지(`scripts/check_contracts.py`), 문서의 코드 블록이 컴파일되는 예제 구간과 같은지(`scripts/check_doc_examples.py`) 본다.

preflight 판정은 `capa_core/test/hosts`에 있는, 손으로 쓴 호스트 스냅샷으로 시험한다. 스냅샷은 일반 x86 데스크톱, JetPack 6의 Orin, 컨테이너, root로 읽은 RT 커널이다. 같은 디렉터리의 `capture.sh`는 지금 호스트의 파일을 읽어 출력하기만 한다. 이 출력은 스냅샷을 쓸 때 참고하는 용도다. 출력에는 호스트를 알아볼 수 있는 정보가 있으므로 그대로 커밋하지 않는다.

`colcon test`는 다음도 돌린다. C++와 Python의 이름이 같은지(`test_surface.py`), 추적하는 stub이 바인딩과 같은지(`test_stub.py`), 문서의 yaml 블록이 읽히는지 본다. fuzz corpus와 위의 문서 검사 셋도 돌린다. `-DCAPA_SANITIZER=thread`(또는 `address,undefined`)는 `capa_core`를 sanitizer로 빌드한다.

## flux와의 연결

`capa_ros`는 flux를 링크한다. `capa::flux` 함수는 `PartitionedExecutor`의 `on_thread_start` hook에 등록한다. 그룹이 무엇을 가졌는지 읽을 때는 `add_to_callback_group`으로 그룹에 넣은 flux 구독도 본다. 컨테이너는 flux `ComponentManager`의 하위 클래스다. `capa.flux`는 executor를 아무 객체로 받으므로 flux를 import하지 않는다. flux는 capa에 의존하지 않고, `capa_core`는 flux에 의존하지 않는다.

이 연결 방식은 언제든 바뀔 수 있다. hook, 직접 링크, 패키지 분할은 지금의 배치이고 계약이 아니다.

## 라이선스

MIT
