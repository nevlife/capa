"""The Python surface is the C++ surface: same names, nothing missing, nothing extra.

The C++ names are read from the public headers (scripts/cxx_names.py), so adding a function on
one side and not the other fails here rather than in a user's port. spawn() is a template in C++
and a function in Python; it is a name on both sides.
"""

import sys
from pathlib import Path

import cyra
import cyra.flux

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))

from cxx_names import core_names, flux_names  # noqa: E402


def test_core_names_match():
    assert set(cyra.__all__) == core_names()


def test_flux_names_match():
    assert set(cyra.flux.__all__) == flux_names()


def test_every_listed_name_exists():
    for module in (cyra, cyra.flux):
        for name in module.__all__:
            assert hasattr(module, name), f"{module.__name__}.{name}"
