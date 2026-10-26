"""capa_cli against real holder processes.

The commands are thin over capa.Spec, capa.preflight, capa.bindings, capa.verify and
capa.verify_process, which capa_core and capa_py test. What is pinned here is what the text and the
exit code say for the states a person runs this in: nothing bound, one holder, two holders of one
stage, a holder the file does not name.
"""

import os
import subprocess
import sys

import pytest

import capa
from capa_cli.main import main

CPU = sorted(os.sched_getaffinity(0))[0]


def spec_file(tmp_path, target="soft"):
    path = tmp_path / "chain.yaml"
    path.write_text(
        "chains:\n"
        "  c:\n"
        f"    target: {target}\n"
        "    stages:\n"
        f"      - {{node: /cli, group: work, cpus: [{CPU}]}}\n"
        f"      - {{node: /cli, group: idle, cpus: [{CPU}]}}\n"
        "      - {node: /dds, group: listener, external: true, expect_priority: 0}\n"
    )
    return str(path)


def holder(spec, node="/cli", group="work"):
    """A process that binds one stage and waits; returns (process, tid)."""
    child = subprocess.Popen(
        [
            sys.executable,
            "-c",
            f"import capa, time; capa.bind({node!r}, {group!r}); "
            "print(capa.this_tid(), flush=True); time.sleep(30)",
        ],
        env={**os.environ, "CAPA_SPEC": spec},
        stdout=subprocess.PIPE,
        text=True,
    )
    return child, int(child.stdout.readline())


def row(out, stage):
    return next(line for line in out.splitlines() if line.startswith(stage + " "))


def test_no_file_is_an_argument_error(monkeypatch, capsys):
    monkeypatch.delenv("CAPA_SPEC", raising=False)
    with pytest.raises(SystemExit) as e:
        main(["status"])
    assert e.value.code == 2
    assert "CAPA_SPEC" in capsys.readouterr().err


def test_a_file_that_breaks_a_rule_ends_the_command(tmp_path, capsys):
    bad = tmp_path / "bad.yaml"
    bad.write_text("chains: {c: {target: soft, stages: [{node: /a, priorty: 3}]}}\n")
    with pytest.raises(SystemExit) as e:
        main(["check", str(bad)])
    assert e.value.code == 1
    assert "unknown key 'priorty'" in capsys.readouterr().err


def test_check_reads_every_stage_with_its_chains_strictness(tmp_path, capsys):
    soft = spec_file(tmp_path, "soft")
    assert main(["check", soft]) == 0
    out = capsys.readouterr().out
    assert "the rules hold" in out
    assert "(soft)" in row(out, "/cli/work")
    assert "external" in row(out, "/dds/listener")

    opts = capa.Options(cpus=[CPU])
    warned = any(f.verdict == capa.Verdict.Warn for f in capa.preflight(opts).findings)
    hard = spec_file(tmp_path, "hard")
    assert main(["check", hard]) == (1 if warned else 0)
    if warned:
        assert "refused (hard)" in row(capsys.readouterr().out, "/cli/work")


def test_status_shows_the_holder_and_what_it_runs_at(tmp_path, capsys):
    spec = spec_file(tmp_path)
    child, tid = holder(spec)
    try:
        assert main(["status", spec]) == 0
        out = capsys.readouterr().out
    finally:
        child.kill()
        child.wait()
    line = row(out, "/cli/work")
    assert f"{child.pid}" in line and f"{tid}" in line and " ok " in line + " "
    assert "unbound" in row(out, "/cli/idle")
    assert "external" in row(out, "/dds/listener")

    assert main(["status", spec]) == 0
    assert "unbound" in row(capsys.readouterr().out, "/cli/work")


def test_two_processes_holding_one_stage_is_a_failure(tmp_path, capsys):
    spec = spec_file(tmp_path)
    first, _ = holder(spec)
    second, _ = holder(spec)
    try:
        assert main(["status", spec]) == 1
        out = capsys.readouterr().out
    finally:
        for p in (first, second):
            p.kill()
            p.wait()
    assert "held by 2 threads" in row(out, "/cli/work")


def test_a_held_stage_the_file_does_not_name_is_shown(tmp_path, capsys):
    spec = spec_file(tmp_path)
    other = tmp_path / "other.yaml"
    other.write_text(
        f"chains: {{o: {{target: soft, stages: [{{node: /elsewhere, cpus: [{CPU}]}}]}}}}\n"
    )
    child, _ = holder(str(other), node="/elsewhere", group="")
    try:
        assert main(["status", spec]) == 0
        out = capsys.readouterr().out
    finally:
        child.kill()
        child.wait()
    assert "not in this file" in row(out, "/elsewhere")


def test_check_reports_the_file_as_a_whole_on_this_host(tmp_path, capsys):
    main(["check", spec_file(tmp_path), "-v"])
    out = capsys.readouterr().out
    assert "the file on this host:" in out
    assert "housekeeping" in out and "foreign-rt-thread" in out


def test_status_reads_counters_over_a_window(tmp_path, capsys):
    spec = spec_file(tmp_path)
    child, _ = holder(spec)
    try:
        main(["status", spec, "-w", "0.1", "-v"])
        out = capsys.readouterr().out
    finally:
        child.kill()
        child.wait()
    for id in ("thread-faults", "cpu-interrupts", "cpu-softirqs", "the host now"):
        assert id in out
