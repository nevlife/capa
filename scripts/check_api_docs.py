#!/usr/bin/env python3
"""Every public name is in the user document, or nothing would catch an undocumented one.

Python has the same names (capa_py/test/test_surface.py), so one list covers both languages.
"""

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from cxx_names import ROOT, core_names, flux_names  # noqa: E402

DOC = ROOT / "docs" / "api.md"


def main():
    wanted = core_names() | flux_names()
    text = DOC.read_text()
    missing = [n for n in sorted(wanted) if not re.search(rf"\b{re.escape(n)}\b", text)]
    if missing:
        print(f"public names missing from {DOC.relative_to(ROOT)} (scripts/check_api_docs.py):")
        for m in missing:
            print(f"  {m}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
