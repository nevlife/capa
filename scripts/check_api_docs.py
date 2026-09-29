#!/usr/bin/env python3
"""Every public name is in both user documents.

A name added to a header breaks nothing when nobody documents it, so nothing would catch it.
Python has the same names (cyra_py/test/test_surface.py), so one list covers both languages.
"""

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from cxx_names import ROOT, core_names, flux_names  # noqa: E402

DOCS = (ROOT / "docs" / "en" / "api.en.md", ROOT / "docs" / "ko" / "api.ko.md")


def main():
    wanted = core_names() | flux_names()
    missing = []
    for doc in DOCS:
        text = doc.read_text()
        for name in sorted(wanted):
            if not re.search(rf"\b{re.escape(name)}\b", text):
                missing.append(f"{doc.relative_to(ROOT)}: {name}")
    if missing:
        print("public names missing from the user documents (scripts/check_api_docs.py):")
        for m in missing:
            print(f"  {m}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
