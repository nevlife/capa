"""Turning capa structs into text. Apart from main so tests read the text without a subprocess."""

import capa

_RANK = {
    capa.Verdict.Ok: 0,
    capa.Verdict.Info: 0,
    capa.Verdict.Unknown: 1,
    capa.Verdict.Warn: 2,
    capa.Verdict.Fail: 3,
}
_WORD = {
    capa.Verdict.Ok: "ok",
    capa.Verdict.Unknown: "unknown",
    capa.Verdict.Info: "info",
    capa.Verdict.Warn: "warn",
    capa.Verdict.Fail: "FAIL",
}


def stage_name(s):
    """`node`, `node/group`, or `name`: the spelling the file and capa's messages use."""
    if s.name:
        return s.name
    return s.node + ("/" + s.group if s.group else "")


def verdict_word(v):
    return _WORD[v]


# Unknown and Info are left out of the one-line summary. Unknown means the file claims nothing
# about the item (verify) or a source was unreadable (preflight), and Info blocks nothing; neither
# is the thread or the host being wrong, and printing them on every row would bury the rows that
# are. --verbose shows them.
def worst(report):
    """Fail, else Warn, else Ok."""
    quiet = (capa.Verdict.Unknown, capa.Verdict.Info)
    seen = [f.verdict for f in report.findings if f.verdict not in quiet]
    return max(seen, key=_RANK.get, default=capa.Verdict.Ok)


def notable(report):
    """The ids that are Warn or Fail."""
    ids = [f.id for f in report.findings if f.verdict in (capa.Verdict.Warn, capa.Verdict.Fail)]
    return ", ".join(ids) if ids else "-"


def scheduling(opts):
    policy = {
        capa.Policy.Inherit: "inherit",
        capa.Policy.Other: "other",
        capa.Policy.Fifo: "fifo",
        capa.Policy.RoundRobin: "rr",
    }[opts.policy]
    prio = f" {opts.priority}" if opts.priority else ""
    cpus = ",".join(str(c) for c in opts.cpus) if opts.cpus else "any"
    return f"{policy}{prio} cpus {cpus}"


def table(rows, headers):
    """Left-aligned columns sized to their contents. Returns a list of lines."""
    widths = [len(h) for h in headers]
    for row in rows:
        for i, cell in enumerate(row):
            widths[i] = max(widths[i], len(cell))
    out = ["  ".join(h.ljust(widths[i]) for i, h in enumerate(headers)).rstrip()]
    out.append("  ".join("-" * widths[i] for i in range(len(headers))))
    for row in rows:
        out.append("  ".join(cell.ljust(widths[i]) for i, cell in enumerate(row)).rstrip())
    return out
