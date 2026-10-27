"""capa.flux: scheduling for the threads of a flux.PartitionedExecutor.

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

import capa  # noqa: E402
import capa.flux  # noqa: E402

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
        node, "/pytest/rt/g", callback=lambda _v: seen.append(capa.current().cpus), fingerprint=FP
    )
    group = MutuallyExclusiveCallbackGroup()
    seen = []

    ex = flux.PartitionedExecutor()
    ex.add(sub, group)
    capa.flux.apply_checked(ex, node, group, capa.Options(cpus=[target]))

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
    spec = capa.Spec.load(_spec_file(tmp_path, target))
    pub = flux.Publisher(node, "/pytest/rt/stage", fingerprint=FP, slot_size=4096, slot_count=8)
    sub = flux.Subscription(
        node,
        "/pytest/rt/stage",
        callback=lambda _v: seen.append(capa.current().cpus),
        fingerprint=FP,
    )
    group = MutuallyExclusiveCallbackGroup()
    seen = []

    ex = flux.PartitionedExecutor()
    ex.add(sub, group)
    capa.flux.apply_checked(ex, node, group, spec.stage(node.get_fully_qualified_name(), "infer"))

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
    node.create_timer(0.01, lambda: seen.append(capa.current().cpus))

    ex = flux.PartitionedExecutor()
    ex.add_node(node)
    capa.flux.apply_checked(ex, node, None, capa.Options(cpus=[target]))

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
    parent_before = capa.current().cpus
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
                seen.setdefault(index, capa.current().cpus)

        return cb

    subs = [
        flux.Subscription(node, f"/pytest/rt/two{i}", callback=record(i), fingerprint=FP)
        for i in range(2)
    ]

    ex = flux.PartitionedExecutor()
    for sub, group in zip(subs, groups):
        ex.add(sub, group)
    capa.flux.apply_checked(ex, node, groups[0], capa.Options(cpus=[left]))
    capa.flux.apply_checked(ex, node, groups[1], capa.Options(cpus=[right]))

    t = spin_in_thread(ex)
    try:
        deadline = time.monotonic() + 5.0
        while len(seen) < 2 and time.monotonic() < deadline:
            for pub in pubs:
                pub.publish(np.full(8, 5, np.uint8))
            time.sleep(0.01)
        assert seen == {0: [left], 1: [right]}
        assert capa.current().cpus == parent_before
    finally:
        ex.stop()
        t.join(timeout=10.0)


# A declaration for a unit no child serves would read as applied and never run. That is the
# silent downgrade the surface exists to prevent, so spin() refuses it.
def test_a_declaration_for_an_unserved_unit_is_refused(node):
    ex = flux.PartitionedExecutor()
    capa.flux.apply_checked(
        ex, node, MutuallyExclusiveCallbackGroup(), capa.Options(cpus=[spare_cpus[0]])
    )
    with pytest.raises(ValueError, match="no thread serves"):
        ex.spin()


# Which of two declarations wins must never be a question of call order.
def test_a_second_declaration_for_one_unit_is_refused(node):
    ex = flux.PartitionedExecutor()
    group = MutuallyExclusiveCallbackGroup()
    capa.flux.apply_checked(ex, node, group, capa.Options(cpus=[spare_cpus[0]]))
    with pytest.raises(ValueError, match="already has a thread-start hook"):
        capa.flux.apply_checked(ex, node, group, capa.Options(cpus=[spare_cpus[0]]))


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
            capa.flux.apply_checked(ex, node, group, capa.Options(cpus=[spare_cpus[0]]))
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
    capa.flux.apply_checked(ex, node, group, capa.Options(capa.Policy.Fifo, 99))

    with pytest.raises((RuntimeError, OSError)):
        ex.spin()
    assert not ran
    del pub


@needs_rtprio
def test_a_granted_policy_reaches_the_child(node):
    pub = flux.Publisher(node, "/pytest/rt/fifo", fingerprint=FP, slot_size=4096, slot_count=8)
    sub = flux.Subscription(
        node, "/pytest/rt/fifo", callback=lambda _v: seen.append(capa.current()), fingerprint=FP
    )
    group = MutuallyExclusiveCallbackGroup()
    seen = []

    ex = flux.PartitionedExecutor()
    ex.add(sub, group)
    capa.flux.apply_checked(ex, node, group, capa.Options(capa.Policy.Fifo, 5))

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


# bind() by label from CAPA_SPEC; the stage is looked up at the call, so a typo raises there.
@needs_two_cpus
def test_bind_reaches_the_group_child(node, tmp_path, monkeypatch):
    target = spare_cpus[-1]
    monkeypatch.setenv("CAPA_SPEC", _spec_file(tmp_path, target))
    pub = flux.Publisher(node, "/pytest/rt/bind", fingerprint=FP, slot_size=4096, slot_count=8)
    sub = flux.Subscription(
        node,
        "/pytest/rt/bind",
        callback=lambda _v: seen.append(capa.current().cpus),
        fingerprint=FP,
    )
    group = MutuallyExclusiveCallbackGroup()
    seen = []

    ex = flux.PartitionedExecutor()
    ex.add(sub, group)
    with pytest.raises(RuntimeError, match="no stage"):
        capa.flux.bind(ex, node, group, "infr")
    capa.flux.bind(ex, node, group, "infer")

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
    spec = capa.Spec.load(_spec_file(tmp_path, spare_cpus[0]))
    ex = flux.PartitionedExecutor()
    with pytest.raises(ValueError, match="but was handed node"):
        capa.flux.apply_checked(ex, node, None, spec.stage("/control_node", "loop"))
    with pytest.raises(ValueError, match="default unit"):
        capa.flux.apply_checked(ex, node, None, spec.stage("/flux_rt_test", "infer"))


def _holds_spec(tmp_path, first, last):
    path = tmp_path / "holds.yaml"
    path.write_text(
        "chains:\n"
        "  c:\n"
        "    target: soft\n"
        "    stages:\n"
        f"      - {{node: /flux_rt_test, cpus: [{first}]}}\n"
        "      - node: /flux_rt_test\n"
        "        group: traj\n"
        "        holds: {subscription: /pytest/holds/traj}\n"
        f"        cpus: [{last}]\n"
    )
    return str(path)


def test_declared_finds_the_group_by_what_it_holds(node, tmp_path):
    from std_msgs.msg import UInt64

    other = MutuallyExclusiveCallbackGroup()
    traj = MutuallyExclusiveCallbackGroup()
    node.create_subscription(UInt64, "/pytest/holds/map", lambda _m: None, 10, callback_group=other)
    node.create_subscription(UInt64, "/pytest/holds/traj", lambda _m: None, 10, callback_group=traj)
    spec = capa.Spec.load(_holds_spec(tmp_path, 0, 0))

    assert capa.flux.content(node, traj).subscriptions == ["/pytest/holds/traj"]
    assert capa.flux.declared(spec, node, traj).group == "traj"
    assert capa.flux.declared(spec, node, other) is None
    assert capa.flux.declared(spec, node, None).group == ""


# rclpy runs every group of a node on the node's one ROS thread here, so the stage of the group
# that holds the subscription is the node thread's.
@needs_two_cpus
def test_bind_declared_reaches_the_node_unit(node, tmp_path, monkeypatch):
    from std_msgs.msg import UInt64

    target = spare_cpus[-1]
    path = tmp_path / "unit.yaml"
    path.write_text(
        "chains: {c: {target: soft, stages: [{node: /flux_rt_test, group: traj, "
        f"holds: {{subscription: /pytest/holds/traj}}, cpus: [{target}]}}]}}}}\n"
    )
    monkeypatch.setenv("CAPA_SPEC", str(path))
    seen = []
    node.create_subscription(
        UInt64,
        "/pytest/holds/traj",
        lambda _m: seen.append(capa.current().cpus),
        10,
        callback_group=MutuallyExclusiveCallbackGroup(),
    )
    pub = node.create_publisher(UInt64, "/pytest/holds/traj", 10)

    ex = flux.PartitionedExecutor()
    ex.add_node(node)
    capa.flux.bind_declared(ex, node)

    t = spin_in_thread(ex)
    try:
        deadline = time.monotonic() + 5.0
        while not seen and time.monotonic() < deadline:
            pub.publish(UInt64())
            time.sleep(0.01)
        assert seen, "the subscription never ran"
        assert seen[0] == [target]
    finally:
        ex.stop()
        t.join(timeout=10.0)


def test_bind_declared_refuses_two_stages_on_the_node_thread(node, tmp_path, monkeypatch):
    from std_msgs.msg import UInt64

    node.create_subscription(
        UInt64,
        "/pytest/holds/traj",
        lambda _m: None,
        10,
        callback_group=MutuallyExclusiveCallbackGroup(),
    )
    monkeypatch.setenv("CAPA_SPEC", _holds_spec(tmp_path, spare_cpus[0], spare_cpus[0]))
    with pytest.raises(ValueError, match="one ROS thread"):
        capa.flux.bind_declared(flux.PartitionedExecutor(), node)
