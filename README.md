# capa (Chain-Aware Priority and Affinity)

capa는 데이터 경로에 있는 ROS 2 노드 스레드의 스케줄링을 관리한다. 체인 위 모든 스레드의 정책, 우선순위, CPU, 자식 프로세스에 상속할 설정은 체인 파일 하나에 선언한다. 체인 파일의 `stages` 항목 하나가 stage이고, stage는 스레드 하나(노드와 그룹, 또는 이름)와 그 스케줄을 정한다.

capa는 이 파일 전체를 검사한다.

- 두 stage가 CPU를 공유하면 downstream stage의 우선순위가 upstream stage보다 높아야 한다.
- release 스레드는 자기가 넘겨주는 작업보다 높아야 한다.
- 더 중요한 체인이 덜 중요한 체인보다 앞선다.

각 스레드는 코드 한 줄로 자기 stage를 적용한다. capa는 적용 전에 호스트 설정을 판정하고, 적용 후에는 커널에서 값을 다시 읽어 확인한다.

```cpp
capa::bind("/control_node", "loop");                     // this thread
auto t = capa::spawn("/perception_node", "infer", fn);   // a new thread, set before fn runs
capa::flux::bind(ex, *node, group, "grab");              // a flux PartitionedExecutor group
```

코드 수정이 불가능한 컴포넌트 노드의 스레드에는 컨테이너가 적용한다.

```bash
CAPA_SPEC=chain.yaml ros2 run capa_ros capa_component_container
```

| 패키지 | 내용 |
| --- | --- |
| `capa_core` | C++17, ROS 없음. 체인 파일(`capa::Spec`), `bind`, `spawn`, `apply_checked`, `preflight`, `verify`, `verify_process` |
| `capa_ros` | ROS 2 콜백 그룹 스레드용. flux `PartitionedExecutor` 그룹에 적용하는 같은 이름의 함수(`capa/flux.hpp`)와 컴포넌트 컨테이너 `capa_component_container` |
| `capa_py` | 같은 이름의 Python(`capa`, `capa.flux`)과 `capa` 명령. `capa check`와 `capa status`는 데몬 없이 읽기만 한다([`docs/cli.md`](docs/cli.md)) |
| `capa_daemon` | Rust로 작성한 `capad`. 파일이 이름으로 지정한 스레드(인터럽트, DDS, SDK 스레드)에 적용하고 reset 자식을 이동한다([`docs/daemon.md`](docs/daemon.md)) |
| `capa_example` | C++ 예제 `preflight`와 `chain` |

API는 [`docs/api.md`](docs/api.md)에, 호스트 지연 측정 방법은 [`docs/latency.md`](docs/latency.md)에 있다.

## 빌드

ROS 2 Humble과 Jazzy를 지원한다.

```bash
git submodule update --init          # thirdparty/nanobind
source /opt/ros/jazzy/setup.bash
cd <workspace>
colcon build --symlink-install
colcon test
```

`capa_core`는 ROS 없이도 빌드된다. `libyaml-cpp-dev`를 설치하고 `cmake -S capa_core -B build`를 실행한다. `-DCAPA_FUZZER=ON -DCMAKE_CXX_COMPILER=clang++`을 주면 libFuzzer용 `fuzz_spec`을 빌드하고, 주지 않으면 ctest가 같은 하니스를 고정 corpus로 실행한다.

## flux와의 연결

- `capa_ros`는 flux를 링크한다. `capa::flux` 함수는 `PartitionedExecutor`의 `on_thread_start` 훅에 등록된다.
- 그룹 내용을 읽을 때는 `add_to_callback_group`으로 그룹에 넣은 flux 구독도 포함한다.
- 컨테이너는 flux `ComponentManager`의 하위 클래스다.
- `capa.flux`는 executor를 임의의 객체로 받으므로 flux를 import하지 않는다.
- flux는 capa에 의존하지 않고, `capa_core`는 flux에 의존하지 않는다.
- 훅, 직접 링크, 패키지 분할 방식은 계약에 포함되지 않는다.

## 라이선스

MIT
