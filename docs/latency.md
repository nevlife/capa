# 지연 측정

capa는 스케줄을 적용하고 검사할 뿐 지연을 측정하지 않는다. 이 문서는 커널의 `rtla`와 `rt-tests`로 호스트 지연을 측정하는 순서다. 측정 전에 `capa check`를 실행한다. 거기 나온 Warn은 측정 결과에 드러날 설정 문제다.

## 계층

아래 계층부터 측정한다. 어느 계층의 지연도 그 아래 계층의 지연보다 작지 않다.

| 계층 | 도구 | 보이는 것 |
| --- | --- | --- |
| 하드웨어 | `rtla hwnoise` | Linux 안에서는 보이지 않는 SMI와 펌웨어 정지 |
| 타이머 인터럽트 | `rtla timerlat`의 IRQ 열 | 타이머 만료부터 핸들러까지 |
| 타이머 스레드 | `rtla timerlat`의 thread 열 | 타이머 만료부터 깨어난 스레드의 실행까지 |
| 사용자 공간 | `cyclictest` | 같은 지연을 사용자 프로세스에서 syscall 경로를 거쳐 측정한 값 |
| CPU 잡음 | `rtla osnoise` | 측정 스레드 외에 그 CPU에서 실행되는 모든 것 |

## 조건

- 부하는 실제 운용과 같은 형태로 준다. CPU, I/O, 네트워크, 메모리 부하(`stress-ng`)나 `rteval`을 사용하고, 측정은 온도가 안정된 뒤 시작한다.
- 마감의 70~80%까지만 사용 가능하다고 보고, p99와 max로 판정한다.
- max는 측정 시간이 길수록 커지므로 같은 길이의 측정끼리만 비교한다. 측정 길이는 점검에 5~10 min, 회귀 확인에 30~60 min, 드문 사건 탐색에 24 h다.
- SSH가 사용하는 NIC의 인터럽트가 측정 CPU로 오지 않게 한다.
- 가상 머신의 결과는 해석하지 않는다(`virtualization`).

## timerlat

`rtla timerlat`은 기본으로 자기 스레드를 FIFO 95로 실행한다. 이 우선순위는 대부분의 stage보다 높아서, timerlat이 보고하는 지연은 stage가 실제로 겪을 지연보다 작다. 그래서 `-P`로 stage의 우선순위를 주고, SCHED_OTHER 기준선은 따로 측정한다.

```text
rtla timerlat hist -c 3 -P f:80 -d 10m -b 1 -E 1000
rtla timerlat hist -c 3 -P o:0 -d 10m -b 1 -E 1000
rtla timerlat top -c 3 -P f:80 -a 100
```

- CI에서는 항상 `-d`를 줘서 측정이 종료되게 한다.
- 히스토그램은 `-b`(구간 폭)와 `-E`(구간 수)를 정해 `over`에 표본이 들어가지 않게 한다.
- `-a <us>`는 한도를 넘는 첫 표본에서 멈추고, 그 사이에 실행된 것을 보여 준다.

## osnoise

```text
rtla osnoise top -c 3 -d 10m
```

격리된 CPU는 `%CPU` available이 99.99% 이상이어야 한다. osnoise에 잡음이 없는데 timerlat의 thread 열이 높으면, 더 높은 RT 스레드나 DEADLINE 태스크(fair server 포함)가 CPU를 차지한다는 뜻이다. 이 경우 `foreign-rt-thread`, `deadline-task`, `rt-throttle` finding을 확인한다.

## cyclictest

```text
cyclictest --mlockall --smp --priority=80 --interval=200 --distance=0 -D 60
trace-cmd record -e irq -e sched -e ipi -- cyclictest --mlockall --smp --priority=80 --interval=200 --distance=0 -D 60
```

- `mpstat -P ALL`에서 `%irq`가 30%나 `%soft`가 20%를 넘는 CPU가 있으면 측정을 더 하기 전에 원인을 찾는다. window를 준 `verify`가 어느 행이 늘었는지 알려 준다(`cpu-interrupts`, `cpu-softirqs`).
- `pip_stress`(rt-tests)로 이 커널에서 PI futex가 동작하는지 확인한다.

## rtla가 없는 커널

Jetson의 5.15 커널에는 osnoise와 timerlat tracer는 있지만 `rtla` 도구가 없다. 커널의 tracer를 확인한 뒤 `/sys/kernel/tracing`의 tracefs를 직접 사용한다.

```text
cd /sys/kernel/tracing
cat available_tracers               # osnoise, timerlat, irqsoff, preemptoff, wakeup_rt
echo 3 > osnoise/cpus
echo 1000 > osnoise/timerlat_period_us
echo timerlat > current_tracer
cat trace_pipe
echo 5 > tracing_thresh             # osnoise: noise로 세는 최소 간격(us)
echo osnoise > current_tracer
```

## 런타임 검증

커널 6.17부터 런타임 검증 모니터 `rtapp`가 있다. 그 아래의 `pagefault` 모니터는 RT 태스크의 page fault를, `sleep` 모니터는 RT 태스크가 잠들면 안 되는 곳에서 잠드는 것을 커널 안에서 보고한다. 이 모니터는 테스트 시에만 활성화한다.
