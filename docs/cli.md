# `capa` 명령

`capa` 명령은 체인 파일을 이 호스트에서 판정한다. 또 각 stage를 어느 스레드가 쥐었는지와 그 스레드가 선언대로 도는지를 보여 준다. 이 명령은 읽기만 한다. 읽는 것은 파일, bind한 프로세스들이 남긴 기록, `/proc`이다. ROS와 데몬이 없어도 돌고, 보고 대상 프로세스는 이 명령이 도는 것을 알아채지 못한다. 명령은 `capa_py`가 `bin/`에 설치한다.

파일은 인자로 주고, 인자가 없으면 `CAPA_SPEC`을 쓴다.

| 종료 코드 | 뜻 |
| --- | --- |
| 0 | 거절될 stage가 없다(`check`). 실패가 없다(`status`) |
| 1 | 파일을 읽지 못했거나 규칙을 어겼다. 거절될 stage가 있거나 파일 전체가 이 호스트에서 실패한다(`check`). 실패가 있다(`status`) |
| 2 | 인자 오류, 또는 파일 미지정 |

요약에는 `ok`, `warn`, `FAIL`만 나온다. `Unknown` finding(읽지 못한 소스, 파일이 선언하지 않은 항목)과 `Info` finding(아무것도 막지 않는 사실)은 요약에서 뺀다. `-v`를 주면 finding을 전부 보여 준다.

## `capa check [FILE] [-v]`

`check`는 먼저 파일을 읽고, 파일이 규칙을 어기면 실패한다. 그다음 capa가 거는 stage마다 `preflight`를 돌리고, 그 결과를 stage의 strictness에 따라 판정한다.

```text
$ capa check chain.yaml
chain.yaml: the rules hold
STAGE              CHAIN      SCHEDULING      HOST            FINDINGS
-----------------  ---------  --------------  --------------  ---------------------------------
/cam/grab          camera     fifo 70 cpus 2  warn (soft)     rt-throttle, cpu-isolation
/control/loop      camera     fifo 90 cpus 3  refused (hard)  policy-permission, rt-throttle
```

`refused`는 `apply_checked`가 거절할 stage라는 뜻이다. `Fail`이 있거나, hard 체인에 `Warn`이 있는 경우다. 권한 판정(`policy-permission`)은 `check`를 돌린 셸을 기준으로 한다. 실제로 bind하는 프로세스는 한도가 다를 수 있다.

표 아래 "the file on this host"에는 `preflight(spec)`의 Warn과 Fail finding이 나온다. stage 하나만 봐서는 보이지 않는 문제들이다. 예를 들어 stage보다 위에서 도는 선언되지 않은 RT 스레드, stage CPU에서 도는 SCHED_DEADLINE 태스크, housekeeping에 남는 CPU가 없는 경우다. 여기서 Fail이 나오면 `check`는 1로 끝난다.

## `capa status [FILE] [-v] [-w SEC]`

`status`는 파일의 stage마다 한 줄을 출력한다. 파일에 없는데 어떤 스레드가 쥔 stage도 하나마다 한 줄을 더 출력한다.

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
| `ok` | 쥔 스레드가 선언대로 돈다(`verify`) |
| `warn` / `FAIL` | `verify`가 선언과 다른 점을 찾았다. DETAIL에 finding 이름이 나온다 |
| `unbound` | 살아 있는 스레드가 이 stage를 쥐지 않았다 |
| `external` | capa가 걸지 않는다. 확인할 기록된 스레드가 없다 |
| `FAIL` held by N threads | 두 프로세스가 한 stage를 쥐었다. 프로세스 안의 검사로는 이것을 볼 수 없다 |
| held, but not in this file | 이 파일이 선언하지 않은 stage를 살아 있는 스레드가 쥐었다 |

데몬 `capad`가 건 stage는 capad가 건 스레드의 pid와 tid로 나온다(`docs/daemon.md`).

표 아래에는 stage를 쥔 프로세스마다 `verify_process`를 돌린 결과가 나온다. 목록에는 stage를 쥐지 않았고 inherit stage의 자식도 아닌 RT 스레드가 나온다. io_uring 도우미 스레드와, hard stage의 CPU에 남은 reset 자식도 나온다.

그다음 "the host now, for the stage cpus"에는 `preflight(spec)`의 결과와, CPU를 고정한 stage마다 `preflight`의 호스트 finding을 지금 다시 읽은 결과가 나온다. preflight는 stage를 bind할 때 한 번만 돈다. 그 뒤에 irqbalance, 전력 데몬, CPU offline이 호스트 설정을 바꿀 수 있다. `status`를 돌린 셸에 관한 finding(권한, 메모리, cgroup, 링크)은 뺀다. 여기서 Fail이 나오면 `status`는 1로 끝난다.

`-w SEC`를 주면 `verify`가 stage를 쥔 스레드의 카운터와, 그 CPU의 인터럽트와 softirq를 SEC 간격으로 두 번 읽는다(`api.md` 4절). 스레드를 하나씩 차례로 재므로, 스레드마다 window 하나가 걸린다.
