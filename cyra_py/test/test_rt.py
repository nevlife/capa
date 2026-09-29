"""cyra from Python: which thread the kernel prefers, and the chain declaration file.

Not real-time. Python is not an RT target, and nothing here claims a bound. What is checked is
that a declared policy and cpu set reach the calling thread, and that a refusal arrives as an
error instead of as a thread running at the wrong priority.

Affinity carries most of the checking because it needs no privilege.
"""

import os
import threading

import pytest

import cyra

spare_cpus = sorted(os.sched_getaffinity(0))
needs_two_cpus = pytest.mark.skipif(
    len(spare_cpus) < 2, reason="needs at least two cpus to move a thread between them"
)


# Asking for nothing does nothing, and says so with an empty report rather than a host verdict
# no caller asked for.
def test_an_empty_request_is_a_no_op():
    before = cyra.current()
    assert cyra.apply_checked(cyra.Options()).findings == []
    after = cyra.current()
    assert (after.policy, after.priority, after.cpus) == (
        before.policy,
        before.priority,
        before.cpus,
    )


# The thread that asked is the thread that changes, and the change is read back from the kernel
# rather than inferred from the call returning.
@needs_two_cpus
def test_affinity_lands_on_the_calling_thread_only():
    target = spare_cpus[0]
    main_before = cyra.current().cpus
    got = {}

    def worker():
        cyra.apply_checked(cyra.Options(cpus=[target]))
        got["cpus"] = cyra.current().cpus
        got["tid"] = cyra.this_tid()

    t = threading.Thread(target=worker)
    t.start()
    t.join(timeout=5.0)

    assert got["cpus"] == [target]
    assert got["tid"] != cyra.this_tid()
    assert cyra.current().cpus == main_before


# A request the kernel cannot satisfy is refused with the reason, not applied partially and not
# downgraded to something weaker.
def test_a_cpu_this_host_does_not_have_is_refused():
    absent = os.cpu_count() + 64
    before = cyra.current().cpus
    with pytest.raises((RuntimeError, OSError)):
        cyra.apply_checked(cyra.Options(cpus=[absent]))
    assert cyra.current().cpus == before


# Validation is the same as the C++ Options::validate: out of range is rejected, not clamped.
def test_an_out_of_range_priority_is_rejected():
    with pytest.raises(ValueError):
        cyra.apply_checked(cyra.Options(policy=cyra.Policy.Fifo, priority=200))


def _spec_file(tmp_path, cpu):
    path = tmp_path / "rt_spec.yaml"
    path.write_text(
        "chains:\n"
        "  camera_to_control:\n"
        "    target: soft\n"
        "    stages:\n"
        "      - node: /flux_rt_test\n"
        "        group: infer\n"
        f"        cpus: [{cpu}]\n"
        "      - node: /control_node\n"
        "        group: loop\n"
        "        policy: fifo\n"
        "        priority: 90\n"
    )
    return str(path)


# Python reads the file through the parser C++ uses, so a stage says the same thing in both.
def test_a_stage_is_looked_up_by_node_and_label(tmp_path):
    spec = cyra.Spec.load(_spec_file(tmp_path, spare_cpus[0]))
    st = spec.stage("/flux_rt_test", "infer")
    assert st.chain == "camera_to_control"
    assert st.opts.cpus == [spare_cpus[0]]
    assert st.opts.children == cyra.Children.Reset
    with pytest.raises(RuntimeError, match="no stage"):
        spec.stage("/flux_rt_test", "infr")


# bind() is the one line a thread needs: the file names its scheduling, the code names the stage.
@needs_two_cpus
def test_bind_puts_the_calling_thread_on_its_stage(tmp_path, monkeypatch):
    target = spare_cpus[-1]
    monkeypatch.setenv("CYRA_SPEC", _spec_file(tmp_path, target))
    got = {}

    def worker():
        cyra.bind("/flux_rt_test", "infer")
        got["cpus"] = cyra.current().cpus

    t = threading.Thread(target=worker)
    t.start()
    t.join(timeout=5.0)
    assert got["cpus"] == [target]


def test_bind_without_a_spec_does_nothing(monkeypatch):
    monkeypatch.delenv("CYRA_SPEC", raising=False)
    assert cyra.bind("/flux_rt_test", "infer").findings == []


# spawn() applies before the function runs, so the first thing the function sees is the stage.
@needs_two_cpus
def test_spawn_runs_the_function_after_the_thread_is_set(tmp_path, monkeypatch):
    target = spare_cpus[-1]
    monkeypatch.setenv("CYRA_SPEC", _spec_file(tmp_path, target))
    seen = []
    t = cyra.spawn("/flux_rt_test", "infer", lambda: seen.append(cyra.current().cpus))
    t.join(timeout=5.0)
    assert seen == [[target]]


def test_spawn_raises_the_refusal_and_never_runs_the_function(tmp_path, monkeypatch):
    monkeypatch.setenv("CYRA_SPEC", _spec_file(tmp_path, spare_cpus[0]))
    ran = []
    with pytest.raises(RuntimeError, match="no stage"):
        cyra.spawn("/flux_rt_test", "infr", lambda: ran.append(1))
    assert not ran


# verify() reads a thread back against the file without setting anything.
def test_verify_reads_the_thread_against_the_stage(tmp_path):
    spec = cyra.Spec.load(_spec_file(tmp_path, spare_cpus[0]))
    st = spec.stage("/flux_rt_test", "infer")
    got = {}

    def worker():
        cyra.apply_checked(st)
        got["report"] = cyra.verify(st, cyra.this_tid())

    t = threading.Thread(target=worker)
    t.start()
    t.join(timeout=5.0)
    rep = got["report"]
    assert rep.ok(), rep.to_string()
    assert rep.find("observed-cpus").verdict == cyra.Verdict.Ok
    assert rep.find("no-such-finding") is None


@pytest.mark.skipif(
    __import__("resource").getrlimit(__import__("resource").RLIMIT_RTPRIO)[0] < 10,
    reason="RLIMIT_RTPRIO too low for SCHED_FIFO on this host",
)
def test_children_follow_the_declared_policy():
    for children, child_policy in (
        (cyra.Children.Reset, os.SCHED_OTHER),
        (cyra.Children.Inherit, os.SCHED_FIFO),
    ):
        got = {}

        def worker():
            cyra.apply_checked(cyra.Options(cyra.Policy.Fifo, 10, [], children))
            got["parent"] = cyra.current()
            inner = threading.Thread(target=lambda: got.setdefault("child", cyra.current()))
            inner.start()
            inner.join()

        t = threading.Thread(target=worker)
        t.start()
        t.join(timeout=5.0)
        assert got["parent"].reset_on_fork == (children == cyra.Children.Reset)
        assert got["child"].policy == child_policy


def test_bind_refuses_an_unknown_label(tmp_path, monkeypatch):
    monkeypatch.setenv("CYRA_SPEC", _spec_file(tmp_path, spare_cpus[0]))
    err = []

    def worker():
        try:
            cyra.bind("/flux_rt_test", "infr")
        except RuntimeError as e:
            err.append(str(e))

    t = threading.Thread(target=worker)
    t.start()
    t.join(timeout=5.0)
    assert err and "no stage" in err[0]


# One stage names one thread: a second live thread taking it is refused, the holder may re-apply,
# and the stage is free again once the holder exits.
def test_one_stage_is_held_by_one_live_thread(tmp_path):
    st = cyra.Spec.load(_spec_file(tmp_path, spare_cpus[0])).stage("/flux_rt_test", "infer")
    applied = threading.Event()
    release = threading.Event()
    seen = {}

    def holder():
        cyra.apply_checked(st)
        cyra.apply_checked(st)
        applied.set()
        release.wait(5.0)

    def second():
        try:
            cyra.apply_checked(st)
            seen["second"] = "applied"
        except ValueError as e:
            seen["second"] = str(e)

    h = threading.Thread(target=holder)
    h.start()
    applied.wait(5.0)
    s = threading.Thread(target=second)
    s.start()
    s.join(timeout=5.0)
    release.set()
    h.join(timeout=5.0)
    assert "one stage declares one thread" in seen["second"]

    after = threading.Thread(target=lambda: cyra.apply_checked(st))
    after.start()
    after.join(timeout=5.0)


def test_an_external_stage_is_refused(tmp_path):
    path = tmp_path / "ext.yaml"
    path.write_text(
        "chains:\n  c:\n    target: soft\n    stages:\n"
        "      - {node: /ext, group: listener, external: true, expect_priority: 60}\n"
    )
    st = cyra.Spec.load(str(path)).stage("/ext", "listener")
    with pytest.raises(ValueError, match="external"):
        cyra.apply_checked(st)


@needs_two_cpus
def test_verify_sees_a_drift_away_from_the_declaration(tmp_path):
    st = cyra.Spec.load(_spec_file(tmp_path, spare_cpus[0])).stage("/flux_rt_test", "infer")
    ready = threading.Event()
    stop = threading.Event()
    got = {}

    def worker():
        cyra.apply_checked(st)
        got["tid"] = cyra.this_tid()
        ready.set()
        stop.wait(5.0)

    t = threading.Thread(target=worker)
    t.start()
    ready.wait(5.0)
    before = cyra.verify(st, got["tid"])
    os.sched_setaffinity(got["tid"], {spare_cpus[-1]})  # somebody else moves the thread
    after = cyra.verify(st, got["tid"])
    stop.set()
    t.join(timeout=5.0)
    assert before.ok(), before.to_string()
    assert after.find("observed-cpus").verdict == cyra.Verdict.Fail, after.to_string()


@pytest.mark.skipif(
    __import__("resource").getrlimit(__import__("resource").RLIMIT_RTPRIO)[0] < 60,
    reason="cyra-cap:rtprio RLIMIT_RTPRIO too low for SCHED_FIFO 60",
)
def test_verify_process_names_only_threads_the_file_cannot_explain(tmp_path):
    path = tmp_path / "scan.yaml"
    path.write_text(
        "chains:\n  scan:\n    target: soft\n    stages:\n"
        "      - {node: /scan, group: worker, policy: fifo, priority: 50}\n"
        "      - {node: /scan, group: pool, policy: fifo, priority: 60, children: inherit}\n"
    )
    spec = cyra.Spec.load(str(path))
    stop = threading.Event()
    tids = {}
    started = threading.Barrier(5)

    def park(key):
        tids[key] = cyra.this_tid()
        started.wait(5.0)
        stop.wait(5.0)

    def holder():
        cyra.apply_checked(spec.stage("/scan", "worker"))
        park("holder")

    def copycat():
        os.sched_setscheduler(0, os.SCHED_FIFO, os.sched_param(50))
        park("copycat")

    def pool():
        cyra.apply_checked(spec.stage("/scan", "pool"))
        helper = threading.Thread(target=park, args=("helper",))
        helper.start()
        park("pool")
        helper.join()

    threads = [threading.Thread(target=f) for f in (holder, copycat, pool)]
    for t in threads:
        t.start()
    started.wait(5.0)
    rep = cyra.verify_process(spec)
    stop.set()
    for t in threads:
        t.join(timeout=5.0)

    def named(key):
        return any(f"tid {tids[key]} " in f.detail for f in rep.findings)

    assert not named("holder"), rep.to_string()
    assert not named("helper"), rep.to_string()
    assert named("copycat"), rep.to_string()
    assert rep.ok()


# The record is what another process reads; the holder here is a separate Python process.
def test_another_process_sees_the_stage_a_thread_holds(tmp_path):
    import subprocess
    import sys

    spec = _spec_file(tmp_path, spare_cpus[0])
    child = subprocess.Popen(
        [
            sys.executable,
            "-c",
            "import cyra, time; cyra.bind('/flux_rt_test', 'infer'); "
            "print(cyra.this_tid(), flush=True); time.sleep(30)",
        ],
        env={**os.environ, "CYRA_SPEC": spec},
        stdout=subprocess.PIPE,
        text=True,
    )
    try:
        tid = int(child.stdout.readline())
        held = [b for b in cyra.bindings() if b.pid == child.pid]
        assert [(b.node, b.group, b.tid) for b in held] == [("/flux_rt_test", "infer", tid)]
        rep = cyra.verify_process(cyra.Spec.load(spec), child.pid)
        assert rep.find("record") is None, rep.to_string()
    finally:
        child.kill()
        child.wait()
    assert not [b for b in cyra.bindings() if b.pid == child.pid]


def test_a_matched_stage_is_refused(tmp_path):
    path = tmp_path / "match.yaml"
    path.write_text(
        "chains: {c: {target: soft, stages: [{name: rx, match: {process: cam, thread: dds.recv}, "
        f"cpus: [{spare_cpus[0]}]}}]}}}}\n"
    )
    st = cyra.Spec.load(str(path)).stage("rx")
    assert st.match.process == "cam" and st.match.thread == "dds.recv"
    with pytest.raises(ValueError, match="daemon"):
        cyra.apply_checked(st)
