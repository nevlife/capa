#!/usr/bin/env python3
"""The code fences of the user document are the compiled example regions, code for code.

A fence written ```cpp doc:<id> or ```python doc:<id> must equal the region between [doc:<id>] and
[doc:/<id>] in an example source, comments and blank lines aside (the document comments in Korean;
the sources may not). The sources are compiled (capa_example) and name-checked
(capa_py/test/test_doc_examples.py), so a fence that drifts from them is a fence nothing checks.
Every region needs a fence, and every fence a region.
"""

import re
import sys
import textwrap
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DOC = ROOT / "docs" / "api.md"
SOURCES = (
    ROOT / "capa_example" / "src" / "doc_api.cpp",
    ROOT / "capa_py" / "examples" / "doc_api.py",
)
FENCE = re.compile(r"^```(cpp|python) doc:([\w-]+)\n(.*?)^```$", re.M | re.S)
REGION = re.compile(
    r"^[ \t]*(?://|#) \[doc:([\w-]+)\]\n(.*?)^[ \t]*(?://|#) \[doc:/\1\]", re.M | re.S
)


def code(text, lang):
    if lang == "cpp":
        text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
        text = re.sub(r"//.*", "", text)
    else:
        text = re.sub(r"#.*", "", text)
    lines = [line.rstrip() for line in textwrap.dedent(text).splitlines()]
    return "\n".join(line for line in lines if line.strip())


def main():
    regions = {}
    for src in SOURCES:
        lang = "cpp" if src.suffix == ".cpp" else "python"
        for rid, body in REGION.findall(src.read_text()):
            regions[rid] = (lang, code(body, lang))
    errors = []
    rel = DOC.relative_to(ROOT)
    fenced = set()
    for lang, rid, body in FENCE.findall(DOC.read_text()):
        fenced.add(rid)
        if rid not in regions:
            errors.append(f"{rel}: fence doc:{rid} has no [doc:{rid}] region in the examples")
            continue
        if regions[rid][0] != lang or regions[rid][1] != code(body, lang):
            errors.append(f"{rel}: fence doc:{rid} differs from its example region")
    for rid in sorted(set(regions) - fenced):
        errors.append(f"{rel}: region [doc:{rid}] has no fence")
    for e in errors:
        print(e, file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
