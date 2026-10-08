"""cyra.flux over a flux2.PartitionedExecutor: the same functions, the same hook.

Affinity needs no privilege, so each test reads the declared cpu set back from inside the
callback, on the child thread that runs it.
"""

import os
import threading
import time

import numpy as np
import pytest

rclpy = pytest.importorskip("rclpy")
flux2 = pytest.importorskip("flux2")

from rclpy.callback_groups import MutuallyExclusiveCallbackGroup  # noqa: E402

import cyra  # noqa: E402
import cyra.flux  # noqa: E402

FP = 0xC1E07B

spare_cpus = sorted(os.sched_getaffinity(0))
needs_two_cpus = pytest.mark.skipif(
    len(spare_cpus) < 2, reason="needs at least two cpus to move a thread between them"
)


@pytest.fixture
def node():
    rclpy.init()
    n = rclpy.create_node("flux2_rt_test")
    yield n
    n.destroy_node()
    rclpy.try_shutdown()


def run_until_seen(ex, pub, seen):
    t = threading.Thread(target=ex.spin, daemon=True)
    t.start()
    try:
        deadline = time.monotonic() + 5.0
        while not seen and time.monotonic() < deadline:
            pub.publish(np.full(8, 3, np.uint8))
            time.sleep(0.01)
    finally:
        ex.stop()
        t.join(timeout=10.0)


def subscribed(node, topic, seen):
    pub = flux2.Publisher(node, topic, fingerprint=FP, slot_size=4096, slot_count=8)
    sub = flux2.Subscription(
        node, topic, callback=lambda _f: seen.append(cyra.current().cpus), fingerprint=FP
    )
    return pub, sub


@needs_two_cpus
def test_flux2_group_child_runs_where_it_was_declared(node):
    target = spare_cpus[-1]
    seen = []
    pub, sub = subscribed(node, "/pytest/rt2/g", seen)
    group = MutuallyExclusiveCallbackGroup()
    ex = flux2.PartitionedExecutor()
    ex.add(sub, group)
    cyra.flux.apply_checked(ex, node, group, cyra.Options(cpus=[target]))
    run_until_seen(ex, pub, seen)
    assert seen, "the callback never ran"
    assert seen[0] == [target]


@needs_two_cpus
def test_flux2_bind_reaches_the_group_child(node, tmp_path, monkeypatch):
    target = spare_cpus[-1]
    spec = tmp_path / "rt2_spec.yaml"
    spec.write_text(
        "chains:\n  c:\n    target: soft\n    stages:\n"
        f"      - node: /flux2_rt_test\n        group: grab\n        cpus: [{target}]\n"
    )
    monkeypatch.setenv("CYRA_SPEC", str(spec))
    seen = []
    pub, sub = subscribed(node, "/pytest/rt2/bind", seen)
    group = MutuallyExclusiveCallbackGroup()
    ex = flux2.PartitionedExecutor()
    ex.add(sub, group)
    cyra.flux.bind(ex, node, group, "grab")
    run_until_seen(ex, pub, seen)
    assert seen, "the callback never ran"
    assert seen[0] == [target]


def test_flux2_declaration_for_an_unserved_unit_is_refused(node):
    ex = flux2.PartitionedExecutor()
    cyra.flux.apply_checked(ex, node, MutuallyExclusiveCallbackGroup(), cyra.Options())
    with pytest.raises(ValueError, match="no thread serves"):
        ex.spin()
