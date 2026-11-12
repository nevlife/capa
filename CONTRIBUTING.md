# capa 기여 안내

capa 코드를 고치는 사람이 알아야 할 검사와 계약표다. 사용법은 [README.md](README.md)와 `docs/`에 있다.

## 검사

검사는 `pre-commit install`로 활성화한다.

| 언어 | 포맷 | 린트 |
| --- | --- | --- |
| C++ | clang-format | clang-tidy |
| Rust(`capa_daemon`) | rustfmt | clippy `pedantic`, `-D warnings` |
| Python | ruff-format | flake8(결함 부류), mypy |
| Markdown | markdownlint `--fix` | markdownlint |
| Shell | | shellcheck |
| YAML | | yamllint |

- clang-tidy는 결함 부류, C++ Core Guidelines, CERT 검사를 활성화하고, `.clang-tidy`에 이유를 적은 검사만 비활성화한다. 빌드가 생성한 `compile_commands.json`이 필요하다.
- clippy는 bridge의 C++ 쪽도 컴파일한다. `unsafe` 블록에는 연산 하나만 넣고 `SAFETY:` 주석을 단다. 이런 블록은 bridge와 시그널 마스크에만 있다.
- `Cargo.lock`이 바뀌면 cargo audit가 RustSec advisory DB와 대조한다. `cargo install cargo-audit`와 네트워크가 필요하다.
- mypy는 추적하는 stub `capa_py/capa/_capa.pyi`로 `capa` 전체를 검사한다.

트리 전체에 걸친 검사는 셋이다.

| 스크립트 | 검사 |
| --- | --- |
| `scripts/check_api_docs.py` | 공개 이름이 전부 사용자 문서에 있다 |
| `scripts/check_contracts.py` | 아래 계약표가 존재하는 테스트를 가리킨다 |
| `scripts/check_doc_examples.py` | 문서의 코드 블록이 컴파일되는 예제 구간과 같다 |

preflight 판정은 `capa_core/test/hosts`의 손으로 쓴 호스트 스냅샷으로 테스트한다. 스냅샷은 일반 x86 데스크톱, JetPack 6의 Orin, 컨테이너, root로 읽은 RT 커널이다. 같은 디렉터리의 `capture.sh`는 현재 호스트의 파일을 읽어 출력만 하며, 스냅샷 작성 시 참고용이다. 출력에 호스트를 식별 가능한 정보가 있으므로 그대로 커밋하지 않는다.

`colcon test`는 다음도 실행한다.

- C++와 Python의 이름 일치(`test_surface.py`)
- 추적하는 stub과 바인딩의 일치(`test_stub.py`)
- 문서의 yaml 블록 읽기
- fuzz corpus와 위의 문서 검사 셋

`-DCAPA_SANITIZER=thread`(또는 `address,undefined`)는 `capa_core`를 sanitizer로 빌드한다.

## 계약

C++와 Python이 함께 약속하는 것을 한 행에 하나씩 적고, 그 약속을 확인하는 양쪽 테스트를 함께 적는다. `scripts/check_contracts.py`는 적힌 테스트가 없거나 id가 겹치면 실패한다.

새 약속은 사용자 문서에 적기 전에 이 표에 행부터 만든다.

### 1. 양쪽

| id | 계약 | C++ | Python |
| --- | --- | --- | --- |
| X-001 | CAPA_SPEC이 없으면 bind는 아무것도 하지 않고 빈 report를 반환한다 | `Bind.WithoutASpecItDoesNothing` | `test_bind_without_a_spec_does_nothing` |
| X-002 | bind는 호출한 스레드에 stage를 적용하고 커널에서 되읽는다 | `Bind.PutsTheCallingThreadOnItsStage` | `test_bind_puts_the_calling_thread_on_its_stage` |
| X-003 | 파일에 없는 라벨은 무시하지 않고 거절한다 | `Bind.ATypoIsRefusedNotIgnored` | `test_bind_refuses_an_unknown_label` |
| X-004 | spawn은 스레드에 stage가 적용된 뒤에만 함수를 실행한다 | `Spawn.RunsTheFunctionOnlyAfterTheThreadIsSet` | `test_spawn_runs_the_function_after_the_thread_is_set` |
| X-005 | 거절된 spawn은 호출자에게 거절을 반환하고 함수를 실행하지 않는다 | `Spawn.ARefusalReachesTheCallerAndTheFunctionNeverRuns` | `test_spawn_raises_the_refusal_and_never_runs_the_function` |
| X-006 | children=reset이면 자식은 SCHED_OTHER로, inherit이면 부모 정책으로 시작한다 | `Rt.ChildrenFollowTheDeclaredPolicy` | `test_children_follow_the_declared_policy` |
| X-007 | 아무것도 요청하지 않는 Options는 판정까지 포함해 아무것도 하지 않는다 | `Rt.ApplyCheckedIsANoOpForOptionsThatAskNothing` | `test_an_empty_request_is_a_no_op` |
| X-008 | 다르게 해석될 요청은 잘라 맞추지 않고 거절한다 | `Rt.ValidateRejectsReinterpretableCombos` | `test_an_out_of_range_priority_is_rejected` |
| X-009 | preflight가 Fail한 요청은 스레드를 건드리기 전에 거절한다 | `Rt.ApplyCheckedRefusesWhatPreflightFails` | `test_a_cpu_this_host_does_not_have_is_refused` |
| X-010 | 한 stage는 살아 있는 스레드 하나가 점유한다. 점유한 스레드는 다시 적용 가능하고, 종료되면 해제된다 | `Spec.OneStageIsHeldByOneLiveThread` | `test_one_stage_is_held_by_one_live_thread` |
| X-011 | stage는 노드 전체 이름과 라벨로 찾고, 없는 라벨은 거절한다 | `Spec.LookupByNodeRejectsAnUnknownLabel` | `test_a_stage_is_looked_up_by_node_and_label` |
| X-012 | external stage는 적용하는 진입점 전부가 거절한다 | `Chain.AnExternalStageIsRefusedByBothEntryPoints` | `test_an_external_stage_is_refused` |
| X-013 | verify는 선언에서 벗어난 스레드를 Fail로 본다 | `Chain.VerifySeesADriftAwayFromTheDeclaration` | `test_verify_sees_a_drift_away_from_the_declaration` |
| X-014 | verify_process는 stage를 점유한 스레드와 inherit 자식을 설명하고 나머지를 보고한다 | `Chain.VerifyProcessNamesOnlyTheThreadsTheFileCannotExplain` | `test_verify_process_names_only_threads_the_file_cannot_explain` |
| X-015 | flux bind로 선언한 stage는 첫 콜백 전에 그룹의 자식 스레드에 적용된다 | `Chain.DeclaredStagesReachTheirThreads` | `test_bind_reaches_the_group_child` |
| X-016 | flux에 넘긴 stage는 함께 넘긴 노드를 가리켜야 한다 | `ScheduleStage.RejectsAStageWhoseNodeIsNotTheNodeHandedOver` | `test_a_stage_must_name_the_node_and_its_unit` |
| X-017 | 어느 자식도 맡지 않는 그룹의 선언은 spin이 거절한다 | `Schedule.ScheduleRejects` | `test_a_declaration_for_an_unserved_unit_is_refused` |
| X-018 | 권한이 허용하는 RT 정책은 flux 자식 스레드에 실제로 적용된다 | `Schedule.ScheduleFifoMatchesPreflight` | `test_a_granted_policy_reaches_the_child` |
| X-019 | 다른 프로세스는 어느 스레드가 stage를 점유했는지 보고, 점유한 프로세스가 종료되면 사라진다 | `Record.AnotherProcessSeesTheStageAThreadHolds` | `test_another_process_sees_the_stage_a_thread_holds` |
| X-020 | 데몬이 match하는 stage는 apply_checked와 bind가 거절한다 | `Bind.AMatchedStageIsRefused` | `test_a_matched_stage_is_refused` |
| X-022 | 파일 전체의 preflight는 hard stage와 격리 CPU가 housekeeping CPU를 남기지 않으면 실패한다 | `Hosts.HousekeepingFailsWhenNothingIsLeft` | `test_preflight_of_a_file_fails_a_host_left_without_housekeeping` |
| X-023 | window를 준 verify는 스레드와 그 CPU의 카운터를 더하고, 주지 않으면 읽지 않는다 | `Verify.AWindowReadsTheThreadsAndItsCpusCounters` | `test_verify_with_a_window_reads_counters` |
| X-024 | Info finding은 적용을 막지 않는다. 일반 호스트에서 hard 요청은 지연에 영향을 주는 finding으로만 거절된다 | `Hosts.AStockHostRefusesAHardChainOnlyForLatencyFindings` | `test_informational_findings_never_block_a_hard_request` |
| X-025 | bind_declared는 파일이 선언한 stage만 적용하고, 선언이 없으면 아무것도 적용하지 않고 없음을 반환한다 | `Bind.BindDeclaredBindsOnlyWhatTheFileDeclares` | `test_bind_declared_binds_only_what_the_file_declares` |
| X-026 | 파일은 이름 없는 그룹을 그것이 가진 구독으로 고르고, 그룹의 순서와 무관하다 | `Container.DeclaredFindsTheGroupByWhatItHolds` | `test_declared_finds_the_group_by_what_it_holds` |
| X-027 | flux bind_declared는 구독을 가진 그룹을 맡은 스레드에 stage를 적용한다 | `Container.BindDeclaredReachesTheGroupThatHoldsTheSubscription` | `test_bind_declared_reaches_the_node_unit` |
| X-028 | match의 arg는 cmdline 원소 하나와 정확히 같아야 한다 | `Spec.AMatchNamesAnInterruptOrAProcessAndThread` | `test_a_match_names_a_process_by_one_argument` |
| X-029 | 권한이 없으면 bind는 capad에 요청하고 최대 5 s 기다린다. capad가 적용하지 않으면 거절한다 | `Bind.WithoutPermissionTheThreadAsksCapadAndIsRefusedWithoutAnAnswer` | `test_bind_without_permission_asks_capad_and_is_refused_without_an_answer` |

### 2. 의도한 차이

이 표에서는 테스트 칸 한쪽만 `-`가 가능하다. 이유는 `docs/api.md` 7절에 있다.

| id | 계약 | C++ | Python |
| --- | --- | --- | --- |
| S-001 | spawn은 C++에서 std::thread, Python에서 시작된 threading.Thread를 반환한다 | `Spawn.RunsTheFunctionOnlyAfterTheThreadIsSet` | `test_spawn_runs_the_function_after_the_thread_is_set` |
| S-002 | 그룹이 노드의 것인지는 C++만 확인한다 | `ScheduleStage.RejectsAGroupOfAnotherNode` | - |
| S-003 | Python에서 group=None은 노드 단위(add_node에 준 노드)를 뜻한다 | - | `test_a_node_child_runs_where_it_was_declared` |
| S-004 | capa.flux.apply_checked는 Options와 Stage를 함수 하나가 받는다 | - | `test_a_stage_reaches_the_group_child` |
| S-005 | Python의 flux bind_declared는 노드 단위 하나에 적용하고, 그 스레드에 stage가 둘이면 거절한다 | - | `test_bind_declared_refuses_two_stages_on_the_node_thread` |
