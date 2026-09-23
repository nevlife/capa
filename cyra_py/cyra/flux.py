"""cyra for flux: scheduling for the threads of a flux.ros.PartitionedExecutor.

Each call registers the executor's `on_thread_start` hook, so the thread serving the unit applies
the declaration to itself before its first callback, and a refusal is the spin() error.
"""

from ._cyra import Policy, apply


def set_thread_scheduling(ex, unit, *, policy=None, priority=0, cpus=()):
    """Declare the OS scheduling for the thread of `ex` that will serve `unit`.

    `unit` is a callback group handed to `add_flux`, or a node handed to `add_ros_node`.

        ex.add_flux(front_camera, control)
        cyra.flux.set_thread_scheduling(ex, control, cpus=[4, 5])

    `cyra.Policy.Fifo` needs RLIMIT_RTPRIO or CAP_SYS_NICE; affinity alone needs neither. One
    declaration per unit, shared with `schedule`.
    """
    policy = Policy.Inherit if policy is None else policy
    cpus = list(cpus)
    ex.on_thread_start(unit, lambda: apply(policy=policy, priority=priority, cpus=cpus))


def schedule(ex, unit, stage):
    """Declare that the thread of `ex` serving `unit` runs as `stage` of the chain file.

        spec = cyra.Spec.load()
        cyra.flux.schedule(ex, group, spec.stage(node.get_fully_qualified_name(), "infer"))

    The stage is taken whole: the chain's target and control-loop priority come with it. The
    stage's node is not checked against the unit, because a flux group does not know its node.
    """
    ex.on_thread_start(unit, lambda: apply(stage))
