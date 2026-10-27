"""The capa functions of the same name, run by the PartitionedExecutor child serving the unit.

`ex` is a flux.PartitionedExecutor. A flux Python group does not know its node, so whether
`group` belongs to `node` is not checked."""

from ._capa import GroupContent, Options, Spec, Stage, Strictness, apply_checked as _apply_checked

__all__ = ["apply_checked", "bind", "bind_declared", "content", "declared"]


def _unit(node, group):
    return node if group is None else group


def bind(ex, node, group, label=""):
    """The stage is looked up now, so a typo raises here rather than at spin()."""
    spec = Spec.load()
    if spec.empty():
        return
    apply_checked(ex, node, group, spec.stage(node.get_fully_qualified_name(), label))


def apply_checked(ex, node, group, what, strict=Strictness.Soft, control_priority=0):
    """capa.apply_checked for the child serving `group`, with Options or a Stage."""
    if isinstance(what, Stage):
        if strict != Strictness.Soft or control_priority != 0:
            raise TypeError("capa: a Stage carries its own strictness; pass only the stage")
        if what.external:
            raise ValueError(
                f"capa: stage {what.node} is declared external, so capa does not set it"
            )
        actual = node.get_fully_qualified_name()
        if what.node != actual:
            raise ValueError(f"capa: stage names node {what.node} but was handed node {actual}")
        if what.holds.empty() and (what.group == "") != (group is None):
            raise ValueError(
                "capa: an empty label names the node's default unit (group=None), and a label "
                "names a group"
            )
        ex.on_thread_start(_unit(node, group), lambda: _apply_checked(what))
        return
    if not isinstance(what, Options):
        raise TypeError("capa: apply_checked takes Options or a Stage")
    what.validate()
    ex.on_thread_start(_unit(node, group), lambda: _apply_checked(what, strict, control_priority))


def _groups(node):
    """The default group, then every other group an rclpy entity of the node names."""
    out = [node.default_callback_group]
    for e in (*node.subscriptions, *node.services, *node.timers, *node.clients):
        if not any(g is e.callback_group for g in out):
            out.append(e.callback_group)
    return out


def content(node, group):
    """What the rclpy `group` (None: the default one) holds. A flux subscription is not in it."""
    g = node.default_callback_group if group is None else group
    return GroupContent(
        is_default=g is node.default_callback_group,
        subscriptions=[s.topic_name for s in node.subscriptions if s.callback_group is g],
        services=[s.service_name for s in node.services if s.callback_group is g],
    )


def declared(spec, node, group):
    """The stage `spec` declares for the rclpy `group` (None: the default one), or None."""
    g = node.default_callback_group if group is None else group
    groups = _groups(node)
    stages = spec.declared(node.get_fully_qualified_name(), [content(node, x) for x in groups])
    return next((st for x, st in zip(groups, stages) if x is g), None)


def bind_declared(ex, node):
    """bind() for the node unit, which runs every rclpy group of the node on one thread.

    The stage is the one declared for any of those groups; two of them would put two stages on
    one thread, which raises. flux groups are bound by label with bind()."""
    spec = Spec.load()
    if spec.empty():
        return
    groups = _groups(node)
    found = spec.declared(node.get_fully_qualified_name(), [content(node, g) for g in groups])
    stages = [st for st in found if st is not None]
    if len(stages) > 1:
        names = ", ".join(st.group or "(default)" for st in stages)
        raise ValueError(
            f"capa: stages {names} of {node.get_fully_qualified_name()} all run on its one "
            "ROS thread here"
        )
    if stages:
        # The stage points into spec, which the hook keeps alive.
        ex.on_thread_start(node, lambda st=stages[0], spec=spec: _apply_checked(st))
