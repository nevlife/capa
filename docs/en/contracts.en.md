# capa contracts

What C++ and Python both promise, each row with the test on each side that holds it. `scripts/check_contracts.py` fails when a named test does not exist, when an id repeats, or when this file and `docs/ko/contracts.ko.md` disagree on a row's id or tests.

A new promise gets a row before it gets a claim in a user document.

## 1. Both sides

| id | Contract | C++ | Python |
| --- | --- | --- | --- |
| X-001 | bind with CAPA_SPEC unset does nothing and returns an empty report | `Bind.WithoutASpecItDoesNothing` | `test_bind_without_a_spec_does_nothing` |
| X-002 | bind puts the calling thread on its stage, read back from the kernel | `Bind.PutsTheCallingThreadOnItsStage` | `test_bind_puts_the_calling_thread_on_its_stage` |
| X-003 | A label the file does not name is refused, never ignored | `Bind.ATypoIsRefusedNotIgnored` | `test_bind_refuses_an_unknown_label` |
| X-004 | spawn runs the function only after the thread is set | `Spawn.RunsTheFunctionOnlyAfterTheThreadIsSet` | `test_spawn_runs_the_function_after_the_thread_is_set` |
| X-005 | A refused spawn raises in the caller and never runs the function | `Spawn.ARefusalReachesTheCallerAndTheFunctionNeverRuns` | `test_spawn_raises_the_refusal_and_never_runs_the_function` |
| X-006 | children=reset starts children SCHED_OTHER; children=inherit passes the policy on | `Rt.ChildrenFollowTheDeclaredPolicy` | `test_children_follow_the_declared_policy` |
| X-007 | Options that ask for nothing do nothing, judgement included | `Rt.ApplyCheckedIsANoOpForOptionsThatAskNothing` | `test_an_empty_request_is_a_no_op` |
| X-008 | A request that would be reinterpreted is refused, not clamped | `Rt.ValidateRejectsReinterpretableCombos` | `test_an_out_of_range_priority_is_rejected` |
| X-009 | What preflight fails is refused before the thread is touched | `Rt.ApplyCheckedRefusesWhatPreflightFails` | `test_a_cpu_this_host_does_not_have_is_refused` |
| X-010 | One stage is held by one live thread; the holder may re-apply; exit releases it | `Spec.OneStageIsHeldByOneLiveThread` | `test_one_stage_is_held_by_one_live_thread` |
| X-011 | A stage is looked up by fully-qualified node and label; an unknown label throws | `Spec.LookupByNodeRejectsAnUnknownLabel` | `test_a_stage_is_looked_up_by_node_and_label` |
| X-012 | An external stage is refused by every applying entry point | `Chain.AnExternalStageIsRefusedByBothEntryPoints` | `test_an_external_stage_is_refused` |
| X-013 | verify fails a thread that moved away from its declaration | `Chain.VerifySeesADriftAwayFromTheDeclaration` | `test_verify_sees_a_drift_away_from_the_declaration` |
| X-014 | verify_process explains stage holders and inherit children, and names the rest | `Chain.VerifyProcessNamesOnlyTheThreadsTheFileCannotExplain` | `test_verify_process_names_only_threads_the_file_cannot_explain` |
| X-015 | flux bind reaches the group's child thread before its first callback | `Chain.DeclaredStagesReachTheirThreads` | `test_bind_reaches_the_group_child` |
| X-016 | A stage handed to flux must name the node it is handed with | `ScheduleStage.RejectsAStageWhoseNodeIsNotTheNodeHandedOver` | `test_a_stage_must_name_the_node_and_its_unit` |
| X-017 | A declaration for a group no child serves is refused by spin | `Schedule.ScheduleRejects` | `test_a_declaration_for_an_unserved_unit_is_refused` |
| X-018 | A granted RT policy reaches the flux child thread | `Schedule.ScheduleFifoMatchesPreflight` | `test_a_granted_policy_reaches_the_child` |
| X-019 | Another process sees which thread holds a stage, until the holder exits | `Record.AnotherProcessSeesTheStageAThreadHolds` | `test_another_process_sees_the_stage_a_thread_holds` |
| X-020 | A stage the daemon matches is refused by apply_checked and bind | `Bind.AMatchedStageIsRefused` | `test_a_matched_stage_is_refused` |
| X-022 | preflight of a whole file fails it when its hard stages and the isolated cpus leave no housekeeping cpu | `Hosts.HousekeepingFailsWhenNothingIsLeft` | `test_preflight_of_a_file_fails_a_host_left_without_housekeeping` |
| X-023 | verify with a window adds the thread's and its cpus' counters, and without one reads none | `Verify.AWindowReadsTheThreadsAndItsCpusCounters` | `test_verify_with_a_window_reads_counters` |
| X-024 | An Info finding never refuses an apply; on a stock host a hard request is refused only for findings that move latency | `Hosts.AStockHostRefusesAHardChainOnlyForLatencyFindings` | `test_informational_findings_never_block_a_hard_request` |

## 2. Deliberate differences

A side may be `-` here, never both. The reasons are in `api.en.md` section 7.

| id | Contract | C++ | Python |
| --- | --- | --- | --- |
| S-001 | spawn returns std::thread in C++ and a started threading.Thread in Python | `Spawn.RunsTheFunctionOnlyAfterTheThreadIsSet` | `test_spawn_runs_the_function_after_the_thread_is_set` |
| S-002 | Only C++ checks that the group belongs to the node | `ScheduleStage.RejectsAGroupOfAnotherNode` | - |
| S-003 | In Python, group=None is the node's unit (a node given to add_node) | - | `test_a_node_child_runs_where_it_was_declared` |
| S-004 | capa.flux.apply_checked is one function taking Options or a Stage | - | `test_a_stage_reaches_the_group_child` |
