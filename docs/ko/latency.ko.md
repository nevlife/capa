# 지연 재기

capa는 스케줄을 걸고 검사할 뿐 지연을 재지 않는다. 이 문서는 커널의 `rtla`와 `rt-tests`로 호스트를 재는 순서다. 먼저 `capa check`를 돌린다. 거기 나온 Warn이 측정에 드러날 설정이다.

## 계층

아래부터 잰다. 어느 계층도 그 아래 계층보다 좋을 수 없다.

| 계층 | 도구 | 보이는 것 |
| --- | --- | --- |
| 하드웨어 | `rtla hwnoise` | Linux 안에서는 보이지 않는 SMI와 펌웨어 정지 |
| 타이머 인터럽트 | `rtla timerlat`의 IRQ 열 | 타이머가 울린 때부터 핸들러까지 |
| 타이머 스레드 | `rtla timerlat`의 thread 열 | 타이머가 울린 때부터 깨어난 스레드가 돌 때까지 |
| 사용자 공간 | `cyclictest` | 같은 것을 사용자 프로세스에서, syscall 경로를 거쳐 |
| CPU 잡음 | `rtla osnoise` | 그 CPU에서 도는 나머지 전부 |

## 조건

- 실제로 돌 모습대로 부하를 준다. CPU, I/O, 네트워크, 메모리 부하(`stress-ng`)나 `rteval`을 쓰고, 온도가 안정된 뒤 시작한다.
- 마감의 70~80%만 쓴다고 보고, p99와 max로 판정한다.
- max는 측정 시간이 길수록 커진다. 같은 길이의 측정끼리만 비교한다. 점검은 5~10분, 회귀는 30~60분, 드문 사건은 24시간이다.
- SSH가 쓰는 NIC의 인터럽트가 재는 CPU로 오지 않게 한다.
- 가상 머신의 결과는 해석하지 않는다(`virtualization`).

## timerlat

`rtla timerlat`은 기본으로 자기 스레드를 FIFO 95로 돌린다. 대부분의 stage보다 위라서, 보고하는 숫자가 stage가 겪을 것보다 낮다. stage의 우선순위를 주고, SCHED_OTHER 기준선은 따로 잰다.

```text
rtla timerlat hist -c 3 -P f:80 -d 10m -b 1 -E 1000
rtla timerlat hist -c 3 -P o:0 -d 10m -b 1 -E 1000
rtla timerlat top -c 3 -P f:80 -a 100
```

- CI에서는 늘 `-d`를 줘서 측정이 끝나게 한다.
- 히스토그램은 `-b`(구간 폭)와 `-E`(구간 수)로 잡아 `over`에 표본이 들어가지 않게 한다.
- `-a <us>`는 한도를 넘는 첫 표본에서 멈추고 그 사이 무엇이 돌았는지 보여 준다.

## osnoise

```text
rtla osnoise top -c 3 -d 10m
```

격리된 CPU는 `%CPU` available이 99.99% 이상이어야 한다. osnoise는 깨끗한데 timerlat thread 열이 높으면, 더 높은 RT 스레드나 DEADLINE 태스크(fair server 포함)가 CPU를 가져간다는 뜻이다. `foreign-rt-thread`, `deadline-task`, `rt-throttle`을 본다.

## cyclictest

```text
cyclictest --mlockall --smp --priority=80 --interval=200 --distance=0 -D 60
trace-cmd record -e irq -e sched -e ipi -- cyclictest --mlockall --smp --priority=80 --interval=200 --distance=0 -D 60
```

`mpstat -P ALL`에서 `%irq`가 30%나 `%soft`가 20%를 넘는 CPU가 있으면 더 재기 전에 원인을 찾는다. window를 준 `verify`가 행 이름을 알려 준다(`cpu-interrupts`, `cpu-softirqs`).

`pip_stress`(rt-tests)로 이 커널에서 PI futex가 동작하는지 확인한다.

## rtla가 없는 커널

Jetson의 5.15에는 osnoise와 timerlat tracer는 있지만 `rtla` 도구가 없다. 커널에 어떤 tracer가 있는지 확인한 뒤 `/sys/kernel/tracing`의 tracefs를 직접 쓴다.

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

6.17부터는 런타임 검증 모니터 `rtapp`가 있다. 그 아래 `pagefault`와 `sleep` 모니터가 RT 태스크가 page fault를 겪거나 잠들면 안 되는 곳에서 잠드는 것을 커널 안에서 보고한다. 시험할 때만 켠다.
