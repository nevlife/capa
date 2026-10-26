"""capa/_capa.pyi is committed for type checkers without a build; it must equal the built stub."""

import os
from pathlib import Path

TRACKED = Path(__file__).resolve().parents[1] / "capa" / "_capa.pyi"


def test_the_tracked_stub_matches_the_binding():
    built = Path(os.environ["CAPA_STUB_BUILT"])
    assert built.read_text() == TRACKED.read_text(), (
        f"capa_py/capa/_capa.pyi is stale: copy {built} over it"
    )
