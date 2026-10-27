#!/usr/bin/env python3
"""The contract table names tests that exist.

A row states a promise and the C++ and Python tests that hold it. A renamed or deleted test would
leave the row claiming a check nothing performs, so every name is looked up in the test sources.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DOC = ROOT / "CONTRIBUTING.md"
ROW = re.compile(r"^\| ([XS]-\d{3}) \| .* \| (`[^`]+`|-) \| (`[^`]+`|-) \|$")


def gtests():
    names = set()
    for path in ROOT.glob("capa_*/test/**/*.cpp"):
        for suite, name in re.findall(r"TEST(?:_F)?\((\w+),\s*(\w+)\)", path.read_text()):
            names.add(f"{suite}.{name}")
    return names


def pytests():
    names = set()
    for path in ROOT.glob("capa_*/test/**/*.py"):
        names.update(re.findall(r"^def (test_\w+)\(", path.read_text(), re.M))
    return names


def rows(doc):
    out = []
    for line in doc.read_text().splitlines():
        m = ROW.match(line)
        if m:
            out.append((m.group(1), m.group(2).strip("`"), m.group(3).strip("`")))
    return out


def main():
    cxx, py = gtests(), pytests()
    errors = []
    table = rows(DOC)
    rel = DOC.relative_to(ROOT)
    ids = [r[0] for r in table]
    for dup in sorted({i for i in ids if ids.count(i) > 1}):
        errors.append(f"{rel}: {dup} appears twice")
    for rid, c, p in table:
        if rid.startswith("X") and "-" in (c, p):
            errors.append(f"{rel}: {rid} is a both-sides row and needs both tests")
        if c == "-" and p == "-":
            errors.append(f"{rel}: {rid} names no test at all")
        if c != "-" and c not in cxx:
            errors.append(f"{rel}: {rid} names gtest {c}, which does not exist")
        if p != "-" and p not in py:
            errors.append(f"{rel}: {rid} names pytest {p}, which does not exist")
    if not table:
        errors.append("no contract rows found; the table format changed")
    for e in errors:
        print(e, file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
