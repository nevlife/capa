"""The Python surface is the C++ surface: same names, nothing missing, nothing extra.

The C++ names are read from the public headers by scripts/cxx_names.py."""

import sys
from pathlib import Path

import capa
import capa.flux

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))

from cxx_names import core_names, flux_names  # noqa: E402


def test_core_names_match():
    assert set(capa.__all__) == core_names()


def test_flux_names_match():
    assert set(capa.flux.__all__) == flux_names()


def test_every_listed_name_exists():
    for module in (capa, capa.flux):
        for name in module.__all__:
            assert hasattr(module, name), f"{module.__name__}.{name}"
