"""Never run: the [doc:] region below is the Python fence of docs/*/api.*.md.

Kept equal by scripts/check_doc_examples.py; capa_py/test/test_doc_examples.py checks the names."""


def doc_python(node, ex, group, run_inference):
    # [doc:python]
    import capa
    import capa.flux

    capa.bind(node.get_fully_qualified_name(), "loop")
    t = capa.spawn("/perception_node", "infer", run_inference)
    capa.flux.bind(ex, node, group, "grab")

    rep = capa.apply_checked(capa.Options(capa.Policy.Fifo, 20, [4]), capa.Strictness.Soft)
    if not rep.ok():
        print(rep.to_string())
    state = capa.current()  # state.policy compares with os.SCHED_FIFO
    # [doc:/python]
    return t, state
