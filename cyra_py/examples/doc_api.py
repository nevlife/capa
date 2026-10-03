"""Never run: the [doc:] region below is the Python fence of docs/*/api.*.md.

Kept equal by scripts/check_doc_examples.py; cyra_py/test/test_doc_examples.py checks the names."""


def doc_python(node, ex, group, run_inference):
    # [doc:python]
    import cyra
    import cyra.flux

    cyra.bind(node.get_fully_qualified_name(), "loop")
    t = cyra.spawn("/perception_node", "infer", run_inference)
    cyra.flux.bind(ex, node, group, "grab")

    rep = cyra.apply_checked(cyra.Options(cyra.Policy.Fifo, 20, [4]), cyra.Strictness.Soft)
    if not rep.ok():
        print(rep.to_string())
    state = cyra.current()  # state.policy compares with os.SCHED_FIFO
    # [doc:/python]
    return t, state
