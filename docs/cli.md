# `capa` 명령

`capa` 명령은 체인 파일을 이 호스트에서 판정하고, 각 stage를 어느 스레드가 점유했는지와 그 스레드가 선언대로 실행되는지를 보여 준다.

- 읽기만 한다. 읽는 것은 파일, bind한 프로세스들이 남긴 기록, `/proc`이다.
- ROS와 데몬 없이 실행된다. 보고 대상 프로세스는 이 명령의 실행을 감지하지 않는다.
- `capa_py`가 `bin/`에 설치한다.

파일은 인자로 주고, 인자가 없으면 `CAPA_SPEC`을 사용한다.

| 종료 코드 | `check` | `status` |
| --- | --- | --- |
| 0 | 거절될 stage가 없다 | 실패가 없다 |
| 1 | 파일을 읽지 못했거나, 규칙을 어겼거나, 거절될 stage가 있거나, 파일 전체가 이 호스트에서 실패한다 | 파일을 읽지 못했거나, 규칙을 어겼거나, 실패가 있다 |
| 2 | 인자 오류 또는 파일 미지정 | 인자 오류 또는 파일 미지정 |

요약에는 `ok`, `warn`, `FAIL`만 나온다. `Unknown` finding(읽지 못한 소스, 파일이 선언하지 않은 항목)과 `Info` finding(아무것도 막지 않는 사실)은 제외한다. `-v`를 주면 finding을 전부 보여 준다.

## `capa check [FILE] [-v]`

`check`는 파일을 읽고, 파일이 규칙을 어기면 실패한다. 이어서 capa가 적용하는 stage마다 `preflight`를 실행하고, 그 결과를 stage의 strictness에 따라 판정한다.

```text
$ capa check chain.yaml
chain.yaml: the rules hold
STAGE              CHAIN      SCHEDULING      HOST            FINDINGS
-----------------  ---------  --------------  --------------  ---------------------------------
/cam/grab          camera     fifo 70 cpus 2  warn (soft)     rt-throttle, cpu-isolation
/control/loop      camera     fifo 90 cpus 3  refused (hard)  policy-permission, rt-throttle
```

- `refused`는 `apply_checked`가 거절할 stage다. `Fail`이 있거나, hard 체인에 `Warn`이 있는 경우다.
- 권한 판정(`policy-permission`)은 `check`를 실행한 셸 기준이다. 실제로 bind하는 프로세스는 한도가 다를 가능성이 있다.

표 아래 "the file on this host"에는 `preflight(spec)`의 Warn과 Fail finding이 나온다. stage 하나만으로는 보이지 않는 문제들이다. 예: stage보다 위에서 실행되는 선언되지 않은 RT 스레드, stage CPU의 SCHED_DEADLINE 태스크, housekeeping에 남는 CPU가 없는 경우. 여기서 Fail이 나오면 `check`는 1로 종료된다.

## `capa status [FILE] [-v] [-w SEC]`

`status`는 파일의 stage마다 한 줄을 출력한다. 파일에 없지만 어떤 스레드가 점유한 stage도 하나마다 한 줄을 더 출력한다.

```text
$ capa status chain.yaml
STAGE        PID      TID      STATE     DETAIL
-----------  -------  -------  --------  ------------------------------
/dds/listen  -        -        external  not set by capa; verify(stage, tid) checks it
/cam/grab    1025542  1025556  FAIL      observed-priority
/ctl/loop    -        -        unbound   no live thread holds this stage
```

| STATE | 뜻 |
| --- | --- |
| `ok` | 점유한 스레드가 선언대로 실행된다(`verify`) |
| `warn` / `FAIL` | `verify`가 선언과 다른 점을 찾았다. DETAIL에 finding 이름이 나온다 |
| `unbound` | 이 stage를 점유한 살아 있는 스레드가 없다 |
| `external` | capa가 적용하지 않는 stage이고, 확인할 기록된 스레드가 없다 |
| `FAIL` held by N threads | 두 프로세스가 한 stage를 점유했다. 프로세스 안의 검사로는 보이지 않는다 |
| held, but not in this file | 이 파일이 선언하지 않은 stage를 살아 있는 스레드가 점유했다 |

데몬 `capad`가 적용한 stage는 capad가 적용한 스레드의 pid와 tid로 나온다(`docs/daemon.md`).

표 아래에는 stage를 점유한 프로세스마다 `verify_process`의 결과가 나온다. 목록에 나오는 것은 다음과 같다.

- stage를 점유하지 않았고 inherit stage의 자식도 아닌 RT 스레드
- io_uring 도우미 스레드
- hard stage의 CPU에 남은 reset 자식

그다음 "the host now, for the stage cpus"에는 `preflight(spec)`의 결과와, CPU를 고정한 stage마다 `preflight`의 호스트 finding을 다시 읽은 결과가 나온다.

- preflight는 stage를 bind할 때 한 번만 실행된다. 그 뒤에 irqbalance, 전력 데몬, CPU offline이 호스트 설정을 바꿀 가능성이 있다.
- `status`를 실행한 셸에 관한 finding(권한, 메모리, cgroup, 링크)은 제외한다.
- 여기서 Fail이 나오면 `status`는 1로 종료된다.

`-w SEC`를 주면 `verify`가 stage를 점유한 스레드의 카운터와 그 CPU의 인터럽트와 softirq를 SEC 간격으로 두 번 읽는다(`api.md` 4절). 스레드를 하나씩 차례로 측정하므로 스레드마다 SEC만큼 걸린다.
