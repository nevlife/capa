"""cyra/_cyra.pyi is committed for type checkers without a build; it must equal the built stub."""

import os
from pathlib import Path

TRACKED = Path(__file__).resolve().parents[1] / "cyra" / "_cyra.pyi"


def test_the_tracked_stub_matches_the_binding():
    built = Path(os.environ["CYRA_STUB_BUILT"])
    assert built.read_text() == TRACKED.read_text(), (
        f"cyra_py/cyra/_cyra.pyi is stale: copy {built} over it"
    )
