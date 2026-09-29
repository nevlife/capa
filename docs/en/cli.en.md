# The `cyra` command

`cyra` checks a chain file against this host and shows which thread holds each stage and whether it runs as declared. It only reads: the file, the records that binding processes keep, and `/proc`. It needs no ROS and no daemon, and the processes it reports on cannot notice it. It is installed to `bin/` by `cyra_cli`.

The file is the argument, or `CYRA_SPEC` when none is given.

| Exit code | Meaning |
| --- | --- |
| 0 | Nothing refused (`check`) or nothing failed (`status`) |
| 1 | The file breaks a rule, a stage would be refused (`check`), or something failed (`status`) |
| 2 | Bad arguments, or no file named at all |

Summaries show `ok`, `warn`, and `FAIL`. An `Unknown` finding (a source that could not be read, or an item the file claims nothing about) is left out of the summary; `-v` prints every finding.

## `cyra check [FILE] [-v]`

Reads the file, which fails the command if it breaks a rule, then runs `preflight` for every stage cyra sets and reads the result with the stage's strictness.

```text
$ cyra check chain.yaml
chain.yaml: the rules hold
STAGE              CHAIN      SCHEDULING      HOST            FINDINGS
-----------------  ---------  --------------  --------------  ---------------------------------
/cam/grab          camera     fifo 70 cpus 2  warn (soft)     rt-throttle, cpu-isolation
/control/loop      camera     fifo 90 cpus 3  refused (hard)  policy-permission, rt-throttle
```

`refused` is what `apply_checked` would do: a `Fail`, or a `Warn` on a hard chain. Permission (`policy-permission`) is judged for the shell that runs `check`; the processes that bind may run with other limits.

## `cyra status [FILE] [-v]`

One row per stage of the file, and one per held stage the file does not name.

```text
$ cyra status chain.yaml
STAGE        PID      TID      STATE     DETAIL
-----------  -------  -------  --------  ------------------------------
/dds/listen  -        -        external  not set by cyra; verify(stage, tid) checks it
/cam/grab    1025542  1025556  FAIL      observed-priority
/ctl/loop    -        -        unbound   no live thread holds this stage
```

| STATE | Meaning |
| --- | --- |
| `ok` | The holder runs as declared (`verify`) |
| `warn` / `FAIL` | `verify` found the holder off its declaration; DETAIL names the findings |
| `unbound` | No live thread holds the stage |
| `external` | cyra does not set it; it has no recorded thread to check |
| `FAIL` held by N threads | Two processes hold one stage. The in-process check cannot see this |
| held, but not in this file | A live holder of a stage this file does not declare |

A stage the daemon `cyrad` set shows with the pid and tid of the thread it set (`docs/en/daemon.en.md`).

Below the table, every process that holds a stage is scanned with `verify_process`: RT threads there that hold no stage and are not an inherit stage's children are listed.
