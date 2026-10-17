"""The cyra functions of the same name, run by the PartitionedExecutor child serving the unit.

`ex` is a flux.PartitionedExecutor. A flux Python group does not know its node, so whether
`group` belongs to `node` is not checked."""

from ._cyra import Options, Spec, Stage, Strictness, apply_checked as _apply_checked

__all__ = ["apply_checked", "bind"]


def _unit(node, group):
    return node if group is None else group


def bind(ex, node, group, label=""):
    """The stage is looked up now, so a typo raises here rather than at spin()."""
    spec = Spec.load()
    if spec.empty():
        return
    apply_checked(ex, node, group, spec.stage(node.get_fully_qualified_name(), label))


def apply_checked(ex, node, group, what, strict=Strictness.Soft, control_priority=0):
    """cyra.apply_checked for the child serving `group`, with Options or a Stage."""
    if isinstance(what, Stage):
        if strict != Strictness.Soft or control_priority != 0:
            raise TypeError("cyra: a Stage carries its own strictness; pass only the stage")
        if what.external:
            raise ValueError(
                f"cyra: stage {what.node} is declared external, so cyra does not set it"
            )
        actual = node.get_fully_qualified_name()
        if what.node != actual:
            raise ValueError(f"cyra: stage names node {what.node} but was handed node {actual}")
        if (what.group == "") != (group is None):
            raise ValueError(
                "cyra: an empty label names the node's default unit (group=None), and a label "
                "names a group"
            )
        ex.on_thread_start(_unit(node, group), lambda: _apply_checked(what))
        return
    if not isinstance(what, Options):
        raise TypeError("cyra: apply_checked takes Options or a Stage")
    what.validate()
    ex.on_thread_start(_unit(node, group), lambda: _apply_checked(what, strict, control_priority))
