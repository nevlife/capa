"""Every cyra name the Python document example uses exists.

The example is never run: it needs a node and an executor. Its text is kept equal to the document
fence by scripts/check_doc_examples.py; this checks the names in it resolve.
"""

import ast
from pathlib import Path

import cyra
import cyra.flux

EXAMPLE = Path(__file__).resolve().parents[1] / "examples" / "doc_api.py"


def dotted(node):
    parts = []
    while isinstance(node, ast.Attribute):
        parts.append(node.attr)
        node = node.value
    if isinstance(node, ast.Name):
        parts.append(node.id)
        return ".".join(reversed(parts))
    return None


def test_every_cyra_name_in_the_example_exists():
    tree = ast.parse(EXAMPLE.read_text())
    used = {d for n in ast.walk(tree) if isinstance(n, ast.Attribute) for d in [dotted(n)] if d}
    names = sorted(u for u in used if u.startswith("cyra."))
    assert names, "the example uses no cyra name; the region moved"
    for name in names:
        obj = {"cyra": cyra}["cyra"]
        for part in name.split(".")[1:]:
            assert hasattr(obj, part), f"{name} does not exist"
            obj = getattr(obj, part)
