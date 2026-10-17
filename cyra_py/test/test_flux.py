"""cyra.flux: scheduling for the threads of a flux.PartitionedExecutor.

Not real-time. What is checked is that a declared policy and cpu set reach the child thread that
was supposed to get them, that a refusal arrives as the spin() error instead of as a thread running
at the wrong priority, and that a declaration which would never apply is refused.

The SCHED_FIFO paths depend on RLIMIT_RTPRIO, so they are asked of the host rather than assumed.
"""

import os
import threading
import time

import numpy as np
import pytest

rclpy = pytest.importorskip("rclpy")
flux = pytest.importorskip("flux")

from rclpy.callback_groups import MutuallyExclusiveCallbackGroup  # noqa: E402

import cyra  # noqa: E402
import cyra.flux  # noqa: E402

FP = 0xC1E07A


def rtprio_limit():
    import resource

    return resource.getrlimit(resource.RLIMIT_RTPRIO)[0]


spare_cpus = sorted(os.sched_getaffinity(0))
needs_two_cpus = pytest.mark.skipif(
    len(spare_cpus) < 2, reason="needs at least two cpus to move a thread between them"
)
needs_rtprio = pytest.mark.skipif(
    rtprio_limit() < 10, reason="RLIMIT_RTPRIO too low for SCHED_FIFO on this host"
)


@pytest.fixture
def node():
    rclpy.init()
    n = rclpy.create_node("flux_rt_test")
    yield n
    n.destroy_node()
    rclpy.try_shutdown()


def spin_in_thread(ex):
    t = threading.Thread(target=ex.spin, daemon=True)
    t.start()
    return t


# A group's child applies its own declaration before running any callback, so what the callback
# observes is already the declared placement.
@needs_two_cpus
def test_a_group_child_runs_where_it_was_declared(node):
    target = spare_cpus[-1]
    pub = flux.Publisher(node, "/pytest/rt/g", fingerprint=FP, slot_size=4096, slot_count=8)
    sub = flux.Subscription(
        node, "/pytest/rt/g", callback=lambda _v: seen.append(cyra.current().cpus), fingerprint=FP
    )
    group = MutuallyExclusiveCallbackGroup()
    seen = []

    ex = flux.PartitionedExecutor()
    ex.add(sub, group)
    cyra.flux.apply_checked(ex, node, group, cyra.Options(cpus=[target]))

    t = spin_in_thread(ex)
    try:
        deadline = time.monotonic() + 5.0
        while not seen and time.monotonic() < deadline:
            pub.publish(np.full(8, 3, np.uint8))
            time.sleep(0.01)
        assert seen, "the callback never ran"
        assert seen[0] == [target]
    finally:
        ex.stop()
        t.join(timeout=10.0)


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


# A stage reaches the child thread the way Options do.
@needs_two_cpus
def test_a_stage_reaches_the_group_child(node, tmp_path):
    target = spare_cpus[-1]
    spec = cyra.Spec.load(_spec_file(tmp_path, target))
    pub = flux.Publisher(node, "/pytest/rt/stage", fingerprint=FP, slot_size=4096, slot_count=8)
    sub = flux.Subscription(
        node,
        "/pytest/rt/stage",
        callback=lambda _v: seen.append(cyra.current().cpus),
        fingerprint=FP,
    )
    group = MutuallyExclusiveCallbackGroup()
    seen = []

    ex = flux.PartitionedExecutor()
    ex.add(sub, group)
    cyra.flux.apply_checked(ex, node, group, spec.stage(node.get_fully_qualified_name(), "infer"))

    t = spin_in_thread(ex)
    try:
        deadline = time.monotonic() + 5.0
        while not seen and time.monotonic() < deadline:
            pub.publish(np.full(8, 3, np.uint8))
            time.sleep(0.01)
        assert seen, "the callback never ran"
        assert seen[0] == [target]
    finally:
        ex.stop()
        t.join(timeout=10.0)


# The other half of the split: a node is a partition here too, so it takes a declaration the
# same way a group does.
@needs_two_cpus
def test_a_node_child_runs_where_it_was_declared(node):
    target = spare_cpus[-1]
    seen = []
    node.create_timer(0.01, lambda: seen.append(cyra.current().cpus))

    ex = flux.PartitionedExecutor()
    ex.add_node(node)
    cyra.flux.apply_checked(ex, node, None, cyra.Options(cpus=[target]))

    t = spin_in_thread(ex)
    try:
        deadline = time.monotonic() + 5.0
        while not seen and time.monotonic() < deadline:
            time.sleep(0.01)
        assert seen, "the timer never fired"
        assert seen[0] == [target]
    finally:
        ex.stop()
        t.join(timeout=10.0)


# Two groups, two declarations, two placements. Nothing leaks between the children or onto the
# thread that called spin().
@needs_two_cpus
def test_two_groups_get_the_cpus_each_declared(node):
    left, right = spare_cpus[0], spare_cpus[-1]
    parent_before = cyra.current().cpus
    pubs = [
        flux.Publisher(node, f"/pytest/rt/two{i}", fingerprint=FP, slot_size=4096, slot_count=8)
        for i in range(2)
    ]
    groups = [MutuallyExclusiveCallbackGroup(), MutuallyExclusiveCallbackGroup()]
    seen = {}
    lock = threading.Lock()

    def record(index):
        def cb(_v):
            with lock:
                seen.setdefault(index, cyra.current().cpus)

        return cb

    subs = [
        flux.Subscription(node, f"/pytest/rt/two{i}", callback=record(i), fingerprint=FP)
        for i in range(2)
    ]

    ex = flux.PartitionedExecutor()
    for sub, group in zip(subs, groups):
        ex.add(sub, group)
    cyra.flux.apply_checked(ex, node, groups[0], cyra.Options(cpus=[left]))
    cyra.flux.apply_checked(ex, node, groups[1], cyra.Options(cpus=[right]))

    t = spin_in_thread(ex)
    try:
        deadline = time.monotonic() + 5.0
        while len(seen) < 2 and time.monotonic() < deadline:
            for pub in pubs:
                pub.publish(np.full(8, 5, np.uint8))
            time.sleep(0.01)
        assert seen == {0: [left], 1: [right]}
        assert cyra.current().cpus == parent_before
    finally:
        ex.stop()
        t.join(timeout=10.0)


# A declaration for a unit no child serves would read as applied and never run. That is the
# silent downgrade the surface exists to prevent, so spin() refuses it.
def test_a_declaration_for_an_unserved_unit_is_refused(node):
    ex = flux.PartitionedExecutor()
    cyra.flux.apply_checked(
        ex, node, MutuallyExclusiveCallbackGroup(), cyra.Options(cpus=[spare_cpus[0]])
    )
    with pytest.raises(ValueError, match="no thread serves"):
        ex.spin()


# Which of two declarations wins must never be a question of call order.
def test_a_second_declaration_for_one_unit_is_refused(node):
    ex = flux.PartitionedExecutor()
    group = MutuallyExclusiveCallbackGroup()
    cyra.flux.apply_checked(ex, node, group, cyra.Options(cpus=[spare_cpus[0]]))
    with pytest.raises(ValueError, match="already has a thread-start hook"):
        cyra.flux.apply_checked(ex, node, group, cyra.Options(cpus=[spare_cpus[0]]))


def test_declaring_after_spin_started_is_refused(node):
    pub = flux.Publisher(node, "/pytest/rt/late", fingerprint=FP, slot_size=4096, slot_count=8)
    sub = flux.Subscription(node, "/pytest/rt/late", callback=lambda _v: None, fingerprint=FP)
    group = MutuallyExclusiveCallbackGroup()
    ex = flux.PartitionedExecutor()
    ex.add(sub, group)

    t = spin_in_thread(ex)
    try:
        deadline = time.monotonic() + 2.0
        while not ex._spinning and time.monotonic() < deadline:
            time.sleep(0.01)
        with pytest.raises(RuntimeError, match="while spinning"):
            cyra.flux.apply_checked(ex, node, group, cyra.Options(cpus=[spare_cpus[0]]))
    finally:
        ex.stop()
        t.join(timeout=10.0)
        del pub


# A child that cannot take its declared policy must end the spin with that error. A thread
# running callbacks at a priority nobody asked for is the outcome this rules out.
@pytest.mark.skipif(rtprio_limit() >= 99, reason="this host grants any RT priority")
def test_a_refused_policy_ends_the_spin_rather_than_running_anyway(node):
    pub = flux.Publisher(node, "/pytest/rt/deny", fingerprint=FP, slot_size=4096, slot_count=8)
    sub = flux.Subscription(
        node, "/pytest/rt/deny", callback=lambda _v: ran.append(1), fingerprint=FP
    )
    group = MutuallyExclusiveCallbackGroup()
    ran = []

    ex = flux.PartitionedExecutor()
    ex.add(sub, group)
    cyra.flux.apply_checked(ex, node, group, cyra.Options(cyra.Policy.Fifo, 99))

    with pytest.raises((RuntimeError, OSError)):
        ex.spin()
    assert not ran
    del pub


@needs_rtprio
def test_a_granted_policy_reaches_the_child(node):
    pub = flux.Publisher(node, "/pytest/rt/fifo", fingerprint=FP, slot_size=4096, slot_count=8)
    sub = flux.Subscription(
        node, "/pytest/rt/fifo", callback=lambda _v: seen.append(cyra.current()), fingerprint=FP
    )
    group = MutuallyExclusiveCallbackGroup()
    seen = []

    ex = flux.PartitionedExecutor()
    ex.add(sub, group)
    cyra.flux.apply_checked(ex, node, group, cyra.Options(cyra.Policy.Fifo, 5))

    t = spin_in_thread(ex)
    try:
        deadline = time.monotonic() + 5.0
        while not seen and time.monotonic() < deadline:
            pub.publish(np.full(8, 1, np.uint8))
            time.sleep(0.01)
        assert seen, "the callback never ran"
        assert seen[0].policy == os.SCHED_FIFO
        assert seen[0].priority == 5
    finally:
        ex.stop()
        t.join(timeout=10.0)


# bind() by label from CYRA_SPEC; the stage is looked up at the call, so a typo raises there.
@needs_two_cpus
def test_bind_reaches_the_group_child(node, tmp_path, monkeypatch):
    target = spare_cpus[-1]
    monkeypatch.setenv("CYRA_SPEC", _spec_file(tmp_path, target))
    pub = flux.Publisher(node, "/pytest/rt/bind", fingerprint=FP, slot_size=4096, slot_count=8)
    sub = flux.Subscription(
        node,
        "/pytest/rt/bind",
        callback=lambda _v: seen.append(cyra.current().cpus),
        fingerprint=FP,
    )
    group = MutuallyExclusiveCallbackGroup()
    seen = []

    ex = flux.PartitionedExecutor()
    ex.add(sub, group)
    with pytest.raises(RuntimeError, match="no stage"):
        cyra.flux.bind(ex, node, group, "infr")
    cyra.flux.bind(ex, node, group, "infer")

    t = spin_in_thread(ex)
    try:
        deadline = time.monotonic() + 5.0
        while not seen and time.monotonic() < deadline:
            pub.publish(np.full(8, 3, np.uint8))
            time.sleep(0.01)
        assert seen, "the callback never ran"
        assert seen[0] == [target]
    finally:
        ex.stop()
        t.join(timeout=10.0)


def test_a_stage_must_name_the_node_and_its_unit(node, tmp_path):
    spec = cyra.Spec.load(_spec_file(tmp_path, spare_cpus[0]))
    ex = flux.PartitionedExecutor()
    with pytest.raises(ValueError, match="but was handed node"):
        cyra.flux.apply_checked(ex, node, None, spec.stage("/control_node", "loop"))
    with pytest.raises(ValueError, match="default unit"):
        cyra.flux.apply_checked(ex, node, None, spec.stage("/flux_rt_test", "infer"))
