"""Public C++ names of capa, read from the headers.

One parser for both checks that need it: capa_py/test/test_surface.py (Python mirrors C++) and
scripts/check_api_docs.py (every name is documented). Top-level declarations only, `detail`
excluded; the headers keep every public declaration at column 0.
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CORE_HEADERS = ("capa_core/include/capa/rt.hpp", "capa_core/include/capa/spec.hpp")
FLUX_HEADERS = ("capa_ros/include/capa/flux.hpp",)

TYPE = re.compile(r"(?:struct|class|enum class)\s+(\w+)")
FUNCTION = re.compile(r"(?:const\s+)?(?:Result<[^(]*>|[\w:<>]+)[\s&*]+(\w+)\(")


def names(headers):
    out = set()
    for header in headers:
        in_detail = False
        for line in (ROOT / header).read_text().splitlines():
            if line.startswith("namespace detail"):
                in_detail = True
                continue
            if in_detail:
                in_detail = not line.startswith("}  // namespace detail")
                continue
            if line.startswith((" ", "\t", "//", "#")):
                continue
            m = TYPE.match(line) or FUNCTION.match(line)
            if m:
                out.add(m.group(1))
    return out


def core_names():
    return names(CORE_HEADERS)


def flux_names():
    return names(FLUX_HEADERS)
