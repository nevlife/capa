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
    assert cyra.apply() == ""
    after = cyra.current()
    assert (after.policy, after.priority, after.cpus) == (
        before.policy, before.priority, before.cpus,
    )


# The thread that asked is the thread that changes, and the change is read back from the kernel
# rather than inferred from the call returning.
@needs_two_cpus
def test_affinity_lands_on_the_calling_thread_only():
    target = spare_cpus[0]
    main_before = cyra.current().cpus
    got = {}

    def worker():
        cyra.apply(cpus=[target])
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
        cyra.apply(cpus=[absent])
    assert cyra.current().cpus == before


# Validation is the same as the C++ Options::validate: out of range is rejected, not clamped.
def test_an_out_of_range_priority_is_rejected():
    with pytest.raises(ValueError):
        cyra.apply(policy=cyra.Policy.Fifo, priority=200)


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
    assert st.cpus == [spare_cpus[0]]
    assert st.control_priority == 90
    with pytest.raises(RuntimeError, match="no stage"):
        spec.stage("/flux_rt_test", "infr")
