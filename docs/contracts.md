# capa 계약

C++와 Python이 함께 약속하는 것을 한 행에 하나씩 적고, 그 약속을 확인하는 양쪽의 테스트를 함께 적는다. `scripts/check_contracts.py`는 적힌 테스트가 없거나 id가 겹치면 실패한다.

새 약속은 사용자 문서에 적기 전에 이 표에 행부터 만든다.

## 1. 양쪽

| id | 계약 | C++ | Python |
| --- | --- | --- | --- |
| X-001 | CAPA_SPEC이 없으면 bind는 아무것도 안 하고 빈 report를 돌려준다 | `Bind.WithoutASpecItDoesNothing` | `test_bind_without_a_spec_does_nothing` |
| X-002 | bind는 호출한 스레드를 stage에 걸고 커널에서 되읽는다 | `Bind.PutsTheCallingThreadOnItsStage` | `test_bind_puts_the_calling_thread_on_its_stage` |
| X-003 | 파일에 없는 라벨은 무시하지 않고 거절한다 | `Bind.ATypoIsRefusedNotIgnored` | `test_bind_refuses_an_unknown_label` |
| X-004 | spawn은 스레드가 걸린 뒤에만 함수를 돌린다 | `Spawn.RunsTheFunctionOnlyAfterTheThreadIsSet` | `test_spawn_runs_the_function_after_the_thread_is_set` |
| X-005 | 거절된 spawn은 호출자에게 던지고 함수를 돌리지 않는다 | `Spawn.ARefusalReachesTheCallerAndTheFunctionNeverRuns` | `test_spawn_raises_the_refusal_and_never_runs_the_function` |
| X-006 | children=reset이면 자식은 SCHED_OTHER로, inherit이면 부모 정책으로 시작한다 | `Rt.ChildrenFollowTheDeclaredPolicy` | `test_children_follow_the_declared_policy` |
| X-007 | 아무것도 요청하지 않는 Options는 판정까지 포함해 아무것도 하지 않는다 | `Rt.ApplyCheckedIsANoOpForOptionsThatAskNothing` | `test_an_empty_request_is_a_no_op` |
| X-008 | 다르게 해석될 요청은 잘라 맞추지 않고 거절한다 | `Rt.ValidateRejectsReinterpretableCombos` | `test_an_out_of_range_priority_is_rejected` |
| X-009 | preflight가 Fail한 요청은 스레드를 건드리기 전에 거절한다 | `Rt.ApplyCheckedRefusesWhatPreflightFails` | `test_a_cpu_this_host_does_not_have_is_refused` |
| X-010 | 한 stage는 살아 있는 스레드 하나가 쥔다. 쥔 스레드는 다시 걸 수 있고 끝나면 풀린다 | `Spec.OneStageIsHeldByOneLiveThread` | `test_one_stage_is_held_by_one_live_thread` |
| X-011 | stage는 노드 전체 이름과 라벨로 찾고, 없는 라벨은 예외를 던진다 | `Spec.LookupByNodeRejectsAnUnknownLabel` | `test_a_stage_is_looked_up_by_node_and_label` |
| X-012 | external stage는 거는 진입점 전부가 거절한다 | `Chain.AnExternalStageIsRefusedByBothEntryPoints` | `test_an_external_stage_is_refused` |
| X-013 | verify는 선언에서 벗어난 스레드를 Fail로 본다 | `Chain.VerifySeesADriftAwayFromTheDeclaration` | `test_verify_sees_a_drift_away_from_the_declaration` |
| X-014 | verify_process는 stage를 쥔 스레드와 inherit 자식을 설명하고 나머지를 보고한다 | `Chain.VerifyProcessNamesOnlyTheThreadsTheFileCannotExplain` | `test_verify_process_names_only_threads_the_file_cannot_explain` |
| X-015 | flux bind로 선언한 stage는 첫 콜백 전에 그룹의 자식 스레드에 걸린다 | `Chain.DeclaredStagesReachTheirThreads` | `test_bind_reaches_the_group_child` |
| X-016 | flux에 넘긴 stage는 함께 넘긴 노드를 가리켜야 한다 | `ScheduleStage.RejectsAStageWhoseNodeIsNotTheNodeHandedOver` | `test_a_stage_must_name_the_node_and_its_unit` |
| X-017 | 어느 자식도 맡지 않는 그룹의 선언은 spin이 거절한다 | `Schedule.ScheduleRejects` | `test_a_declaration_for_an_unserved_unit_is_refused` |
| X-018 | 권한이 허용하는 RT 정책은 flux 자식 스레드에 실제로 걸린다 | `Schedule.ScheduleFifoMatchesPreflight` | `test_a_granted_policy_reaches_the_child` |
| X-019 | 다른 프로세스는 어느 스레드가 stage를 쥐었는지 보고, 쥔 프로세스가 끝나면 사라진다 | `Record.AnotherProcessSeesTheStageAThreadHolds` | `test_another_process_sees_the_stage_a_thread_holds` |
| X-020 | 데몬이 match하는 stage는 apply_checked와 bind가 거절한다 | `Bind.AMatchedStageIsRefused` | `test_a_matched_stage_is_refused` |
| X-022 | 파일 전체의 preflight는 hard stage와 격리 CPU가 housekeeping CPU를 남기지 않으면 실패한다 | `Hosts.HousekeepingFailsWhenNothingIsLeft` | `test_preflight_of_a_file_fails_a_host_left_without_housekeeping` |
| X-023 | window를 준 verify는 스레드와 그 CPU의 카운터를 더하고, 주지 않으면 읽지 않는다 | `Verify.AWindowReadsTheThreadsAndItsCpusCounters` | `test_verify_with_a_window_reads_counters` |
| X-024 | Info finding은 적용을 막지 않는다. 일반 호스트에서 hard 요청은 지연에 영향을 주는 finding으로만 거절된다 | `Hosts.AStockHostRefusesAHardChainOnlyForLatencyFindings` | `test_informational_findings_never_block_a_hard_request` |
| X-025 | bind_declared는 파일이 선언한 stage만 걸고, 선언이 없으면 아무것도 걸지 않고 없음을 돌려준다 | `Bind.BindDeclaredBindsOnlyWhatTheFileDeclares` | `test_bind_declared_binds_only_what_the_file_declares` |
| X-026 | 파일은 이름 없는 그룹을 그것이 가진 구독으로 고르고, 그룹의 순서와 무관하다 | `Container.DeclaredFindsTheGroupByWhatItHolds` | `test_declared_finds_the_group_by_what_it_holds` |
| X-027 | flux bind_declared는 구독을 가진 그룹을 맡은 스레드에 stage를 건다 | `Container.BindDeclaredReachesTheGroupThatHoldsTheSubscription` | `test_bind_declared_reaches_the_node_unit` |
| X-028 | match의 arg는 cmdline 원소 하나와 정확히 같아야 한다 | `Spec.AMatchNamesAnInterruptOrAProcessAndThread` | `test_a_match_names_a_process_by_one_argument` |

## 2. 의도한 차이

이 표에서는 테스트 칸 한쪽이 `-`일 수 있지만, 양쪽 다 `-`일 수는 없다. 이유는 `api.md` 7절에 있다.

| id | 계약 | C++ | Python |
| --- | --- | --- | --- |
| S-001 | spawn은 C++에서 std::thread, Python에서 시작된 threading.Thread를 준다 | `Spawn.RunsTheFunctionOnlyAfterTheThreadIsSet` | `test_spawn_runs_the_function_after_the_thread_is_set` |
| S-002 | 그룹이 노드의 것인지는 C++만 확인한다 | `ScheduleStage.RejectsAGroupOfAnotherNode` | - |
| S-003 | Python에서 group=None은 노드 단위(add_node에 준 노드)를 뜻한다 | - | `test_a_node_child_runs_where_it_was_declared` |
| S-004 | capa.flux.apply_checked는 Options와 Stage를 함수 하나가 받는다 | - | `test_a_stage_reaches_the_group_child` |
| S-005 | Python의 flux bind_declared는 노드 단위 하나에 걸고, 그 스레드에 stage 둘이면 거절한다 | - | `test_bind_declared_refuses_two_stages_on_the_node_thread` |
