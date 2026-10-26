"""Argument parsing and the two commands.

Both only read: the chain file, the records other processes keep, and /proc. Nothing here takes
a lock or writes, so running it against a live system cannot perturb what it reports.
"""

import argparse
import datetime
import os
import signal
import sys

import capa

from .format import notable, scheduling, stage_name, table, verdict_word, worst

# What a preflight run from this shell says about this shell, not about the host or a holder.
_THIS_PROCESS = {
    "policy-permission",
    "priority-order",
    "memory-lock",
    "cpuset",
    "cgroup-cpu",
    "cgroup-memory",
    "secure-exec",
    "bind-now",
}


def _spec(path):
    """The file to read: the argument, else CAPA_SPEC. A file that breaks a rule ends the command."""
    path = path or os.environ.get("CAPA_SPEC", "")
    if not path:
        print("capa: no chain file: pass one, or set CAPA_SPEC", file=sys.stderr)
        raise SystemExit(2)
    try:
        return path, capa.Spec.load(path)
    except RuntimeError as e:
        print(str(e), file=sys.stderr)
        raise SystemExit(1)


def _blocks(stage, report):
    """Whether this report would refuse the stage's apply, read with the stage's strictness."""
    if not report.ok():
        return True
    hard = stage.strict == capa.Strictness.Hard
    return hard and any(f.verdict == capa.Verdict.Warn for f in report.findings)


def cmd_check(args):
    path, spec = _spec(args.spec)
    rows = []
    blocked = 0
    for s in spec.stages():
        if s.external:
            rows.append([stage_name(s), s.chain, "external", "-", "not set by capa"])
            continue
        rep = capa.preflight(s.opts, strict=s.strict)
        stops = _blocks(s, rep)
        blocked += stops
        target = "hard" if s.strict == capa.Strictness.Hard else "soft"
        verdict = "refused" if stops else verdict_word(worst(rep))
        rows.append(
            [stage_name(s), s.chain, scheduling(s.opts), f"{verdict} ({target})", notable(rep)]
        )
    print(f"{path}: the rules hold")
    print("\n".join(table(rows, ["STAGE", "CHAIN", "SCHEDULING", "HOST", "FINDINGS"])))
    print()
    print("permission is judged for this shell; the processes that bind may run with other limits.")
    whole = capa.preflight(spec)
    _print_findings("the file on this host:", whole.findings, args.verbose)
    if args.verbose:
        for s in spec.stages():
            if not s.external:
                print(
                    f"\n{stage_name(s)}\n{capa.preflight(s.opts, strict=s.strict).to_string()}",
                    end="",
                )
    return 1 if blocked or not whole.ok() else 0


def _print_findings(title, findings, verbose):
    """Warn and Fail findings, or every one with --verbose; nothing when there is nothing to say."""
    shown = [f for f in findings if verbose or f.verdict in (capa.Verdict.Warn, capa.Verdict.Fail)]
    if shown:
        print(f"\n{title}")
        for f in shown:
            print(f"  {verdict_word(f.verdict)} {f.id}: {f.detail}")


def _host_drift(spec):
    """Host findings for the stage cpus, as they stand now: preflight ran once, at bind."""
    seen = {}
    for f in capa.preflight(spec).findings:
        seen[(f.id, f.detail)] = f
    for s in spec.stages():
        if s.external or not s.opts.cpus:
            continue
        for f in capa.preflight(s.opts, strict=s.strict).findings:
            if f.id not in _THIS_PROCESS:
                seen[(f.id, f.detail)] = f
    return list(seen.values())


def cmd_status(args):
    _, spec = _spec(args.spec)
    held = {}
    for b in capa.bindings():
        held.setdefault(stage_name(b), []).append(b)

    rows = []
    details = []
    failed = False
    declared = set()
    for s in spec.stages():
        name = stage_name(s)
        declared.add(name)
        holders = held.get(name, [])
        if s.external:
            rows.append(
                [name, "-", "-", "external", "not set by capa; verify(stage, tid) checks it"]
            )
        elif not holders:
            rows.append([name, "-", "-", "unbound", "no live thread holds this stage"])
        elif len(holders) > 1:
            failed = True
            where = ", ".join(f"{b.pid}/{b.tid}" for b in holders)
            rows.append([name, "-", "-", "FAIL", f"held by {len(holders)} threads ({where})"])
        else:
            b = holders[0]
            rep = capa.verify(s, b.tid, datetime.timedelta(seconds=args.window))
            failed = failed or not rep.ok()
            rows.append([name, str(b.pid), str(b.tid), verdict_word(worst(rep)), notable(rep)])
            details.append(f"\n{name} (pid {b.pid}, tid {b.tid})\n{rep.to_string()}")
    for name in sorted(set(held) - declared):
        for b in held[name]:
            rows.append([name, str(b.pid), str(b.tid), "warn", "held, but not in this file"])
    print("\n".join(table(rows, ["STAGE", "PID", "TID", "STATE", "DETAIL"])))

    strays = []
    for pid in sorted({b.pid for bs in held.values() for b in bs}):
        rep = capa.verify_process(spec, pid)
        failed = failed or not rep.ok()
        for f in rep.findings:
            if f.verdict != capa.Verdict.Ok:
                strays.append(f"  pid {pid}: {verdict_word(f.verdict)} {f.id}: {f.detail}")
    if strays:
        print("\nthreads the file does not explain, in the processes above:")
        print("\n".join(strays))
    drift = _host_drift(spec)
    failed = failed or any(f.verdict == capa.Verdict.Fail for f in drift)
    _print_findings("the host now, for the stage cpus:", drift, args.verbose)
    if args.verbose:
        print("".join(details), end="")
    return 1 if failed else 0


def build_parser():
    parser = argparse.ArgumentParser(
        prog="capa",
        description="Check a chain file against this host and show what the threads that hold "
        "its stages run at. Reads only; needs no ROS and no daemon.",
    )
    verbs = parser.add_subparsers(dest="verb", required=True)

    check = verbs.add_parser("check", help="The file's rules, and preflight for every stage.")
    check.add_argument("spec", nargs="?", help="Chain file (default: CAPA_SPEC).")
    check.add_argument("-v", "--verbose", action="store_true", help="Every finding, per stage.")
    check.set_defaults(func=cmd_check)

    status = verbs.add_parser("status", help="Which thread holds each stage, and how it runs.")
    status.add_argument("spec", nargs="?", help="Chain file (default: CAPA_SPEC).")
    status.add_argument("-v", "--verbose", action="store_true", help="Every finding, per holder.")
    status.add_argument(
        "-w",
        "--window",
        type=float,
        default=0.0,
        metavar="SEC",
        help="Also read each holder's and its cpus' counters SEC apart (default 0: no counters).",
    )
    status.set_defaults(func=cmd_status)
    return parser


def main(argv=None):
    # `capa status | head` closes the pipe early; a reporting tool should just stop.
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    args = build_parser().parse_args(argv)
    return args.func(args)
