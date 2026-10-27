# capa (Chain-Aware Priority and Affinity)

capa는 데이터가 지나가는 경로에 있는 ROS 2 노드의 스레드를 스케줄링한다. 체인 위의 모든 스레드가 어떻게 도는지는 파일 하나에 선언한다. 선언하는 것은 정책, 우선순위, CPU, 그리고 자식에게 물려줄 것이다.

capa는 이 파일을 통째로 검사한다. 두 stage가 CPU를 공유하면 하류가 상류보다 높아야 한다. release 스레드는 자기가 풀어 주는 작업보다 높아야 한다. 더 중요한 체인이 덜 중요한 체인보다 앞선다.

각 스레드는 코드 한 줄로 자기 stage를 건다. capa는 걸기 전에 호스트 설정을 판정하고, 건 뒤에는 커널에서 값을 다시 읽어 확인한다.

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

## flux와의 연결

`capa_ros`는 flux를 링크한다. `capa::flux` 함수는 `PartitionedExecutor`의 `on_thread_start` hook에 등록한다. 그룹이 무엇을 가졌는지 읽을 때는 `add_to_callback_group`으로 그룹에 넣은 flux 구독도 본다. 컨테이너는 flux `ComponentManager`의 하위 클래스다. `capa.flux`는 executor를 아무 객체로 받으므로 flux를 import하지 않는다. flux는 capa에 의존하지 않고, `capa_core`는 flux에 의존하지 않는다.

이 연결 방식은 언제든 바뀔 수 있다. hook, 직접 링크, 패키지 분할은 지금의 배치이고 계약이 아니다.

## 라이선스

MIT
