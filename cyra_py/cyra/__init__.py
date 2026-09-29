"""cyra: thread scheduling and the chain declaration file for ROS 2 nodes."""

import threading

from ._cyra import (
    Binding,
    Children,
    Finding,
    Kind,
    Match,
    Options,
    Policy,
    Report,
    Spec,
    Stage,
    Strictness,
    ThreadState,
    Verdict,
    apply_checked,
    bind,
    bindings,
    current,
    observe,
    preflight,
    this_tid,
    verify,
    verify_process,
)

__all__ = [
    "Binding",
    "Children",
    "Finding",
    "Kind",
    "Match",
    "Options",
    "Policy",
    "Report",
    "Spec",
    "Stage",
    "Strictness",
    "ThreadState",
    "Verdict",
    "apply_checked",
    "bind",
    "bindings",
    "current",
    "observe",
    "preflight",
    "spawn",
    "this_tid",
    "verify",
    "verify_process",
]


def spawn(target, group, fn):
    """A started thread that runs `fn` as the stage for `target` and `group`.

    Returns once the thread is set. If bind() refuses, the thread ends without running `fn` and
    the refusal is raised here.
    """
    bound = threading.Event()
    refusal = []

    def run():
        try:
            bind(target, group)
        except BaseException as e:
            refusal.append(e)
            bound.set()
            return
        bound.set()
        fn()

    t = threading.Thread(target=run)
    t.start()
    bound.wait()
    if refusal:
        t.join()
        raise refusal[0]
    return t
