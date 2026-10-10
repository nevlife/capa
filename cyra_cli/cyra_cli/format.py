"""Turning cyra structs into text. Apart from main so tests read the text without a subprocess."""

import cyra

_RANK = {
    cyra.Verdict.Ok: 0,
    cyra.Verdict.Info: 0,
    cyra.Verdict.Unknown: 1,
    cyra.Verdict.Warn: 2,
    cyra.Verdict.Fail: 3,
}
_WORD = {
    cyra.Verdict.Ok: "ok",
    cyra.Verdict.Unknown: "unknown",
    cyra.Verdict.Info: "info",
    cyra.Verdict.Warn: "warn",
    cyra.Verdict.Fail: "FAIL",
}


def stage_name(s):
    """`node`, `node/group`, or `name`: the spelling the file and cyra's messages use."""
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
    quiet = (cyra.Verdict.Unknown, cyra.Verdict.Info)
    seen = [f.verdict for f in report.findings if f.verdict not in quiet]
    return max(seen, key=_RANK.get, default=cyra.Verdict.Ok)


def notable(report):
    """The ids that are Warn or Fail."""
    ids = [f.id for f in report.findings if f.verdict in (cyra.Verdict.Warn, cyra.Verdict.Fail)]
    return ", ".join(ids) if ids else "-"


def scheduling(opts):
    policy = {
        cyra.Policy.Inherit: "inherit",
        cyra.Policy.Other: "other",
        cyra.Policy.Fifo: "fifo",
        cyra.Policy.RoundRobin: "rr",
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
