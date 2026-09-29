//! One pass of the daemon over a chain file: set the threads it matches, put a drifted one back,
//! move reset children off their stage's cpus, report drift on the stages threads bind themselves,
//! and record what it holds so `cyra status` sees it. Everything the daemon decides beyond the
//! core's verdicts is here; main only loops, reloads and stops.

use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};

use cxx::UniquePtr;

use crate::ffi::{self, Binding, Match, Spec, Stage, ThreadState};

/// Where the kernel's tables are read from. Tests point these at fake trees.
#[derive(Clone, Debug)]
pub struct Roots {
    pub proc: PathBuf,
    pub sys_irq: PathBuf,
}

impl Default for Roots {
    fn default() -> Self {
        Self {
            proc: PathBuf::from("/proc"),
            sys_irq: PathBuf::from("/sys/kernel/irq"),
        }
    }
}

/// The first line, as bytes: a comm cut at 15 bytes can end inside a UTF-8 sequence.
fn first_line(path: &Path) -> Vec<u8> {
    let mut text = fs::read(path).unwrap_or_default();
    if let Some(end) = text.iter().position(|&b| b == b'\n') {
        text.truncate(end);
    }
    text
}

fn numeric_entries(dir: &Path) -> Vec<i32> {
    let Ok(entries) = fs::read_dir(dir) else {
        return Vec::new();
    };
    let mut out: Vec<i32> = entries
        .filter_map(|e| e.ok()?.file_name().to_str()?.parse().ok())
        .collect();
    out.sort_unstable();
    out
}

/// The kernel keeps a user thread's comm to 15 bytes, so a longer name compares by its first 15.
fn comm_of(name: &str) -> &[u8] {
    &name.as_bytes()[..name.len().min(15)]
}

fn at(pid: i32, tid: i32) -> String {
    format!("pid {pid} tid {tid}")
}

/// Every (pid, tid) a matched stage names. The daemon sets a stage only when this is exactly one.
pub fn find_threads(m: &Match, roots: &Roots) -> Vec<(i32, i32)> {
    let mut out = Vec::new();
    if !m.irq.is_empty() {
        let procs = numeric_entries(&roots.proc);
        for irq in numeric_entries(&roots.sys_irq) {
            let actions = first_line(&roots.sys_irq.join(irq.to_string()).join("actions"));
            if !actions.split(|&b| b == b',').any(|h| h == m.irq.as_bytes()) {
                continue;
            }
            // The handler's thread is irq/<n>-<name>. Kernels since 5.17 show a kernel thread's
            // whole name in comm; older ones cut it to 15 bytes like any thread. irq/<n>-s-<name>
            // is a secondary thread of the same handler and matches neither.
            let want = format!("irq/{irq}-{}", m.irq);
            for &pid in &procs {
                let comm = first_line(&roots.proc.join(pid.to_string()).join("comm"));
                if comm == want.as_bytes() || comm == comm_of(&want) {
                    out.push((pid, pid));
                }
            }
        }
        return out;
    }
    for pid in numeric_entries(&roots.proc) {
        let dir = roots.proc.join(pid.to_string());
        if first_line(&dir.join("comm")) != comm_of(&m.process) {
            continue;
        }
        let tasks = dir.join("task");
        for tid in numeric_entries(&tasks) {
            if first_line(&tasks.join(tid.to_string()).join("comm")) == comm_of(&m.thread) {
                out.push((pid, tid));
            }
        }
    }
    out
}

struct Owned {
    pid: i32,
    tid: i32,
    start: u64,
    original: ThreadState,
    matcher: Match,
}

pub struct Reconciler {
    spec: UniquePtr<Spec>,
    stages: Vec<Stage>,
    roots: Roots,
    owned: BTreeMap<String, Owned>,
    last: BTreeMap<String, String>,
}

impl Reconciler {
    pub fn load(path: &str) -> Result<Self, cxx::Exception> {
        Self::with_roots(path, Roots::default())
    }

    pub fn with_roots(path: &str, roots: Roots) -> Result<Self, cxx::Exception> {
        let spec = ffi::load(path)?;
        let stages = ffi::stages(&spec);
        Ok(Self {
            spec,
            stages,
            roots,
            owned: BTreeMap::new(),
            last: BTreeMap::new(),
        })
    }

    /// One pass. Returns what changed since the last pass, one line each.
    pub fn pass(&mut self) -> Vec<String> {
        let mut out = Vec::new();
        let held = ffi::bindings();
        for (index, st) in self.stages.clone().iter().enumerate() {
            if st.external {
                continue;
            }
            if st.matcher.is_empty() {
                self.watch_bound(index, st, &held, &mut out);
            } else {
                self.set_matched(index, st, &mut out);
            }
        }
        let mine: Vec<Binding> = self
            .owned
            .iter()
            .filter_map(|(key, o)| {
                let st = self.stages.iter().find(|s| &s.key == key)?;
                Some(Binding {
                    node: st.node.clone(),
                    group: st.group.clone(),
                    name: st.name.clone(),
                    pid: o.pid,
                    tid: o.tid,
                })
            })
            .collect();
        let why = ffi::record(&mine);
        if !why.is_empty() {
            let state = format!("not written ({why}); cyra status cannot see these stages");
            self.note(&mut out, "record", state);
        }
        out
    }

    /// Put every thread this daemon set back to what it ran at before, and drop its record.
    pub fn restore(&mut self) -> Vec<String> {
        let owned = std::mem::take(&mut self.owned);
        let out = owned.iter().map(|(key, o)| put_back(key, o)).collect();
        ffi::record(&[]);
        out
    }

    /// Take over the threads `older` set for stages this file still matches the same way; put the
    /// rest back. How a reload avoids dropping every matched thread to its default in between.
    pub fn adopt(&mut self, older: &mut Reconciler) -> Vec<String> {
        let mut out = Vec::new();
        for (key, o) in std::mem::take(&mut older.owned) {
            let same = self
                .stages
                .iter()
                .any(|s| s.key == key && s.matcher == o.matcher);
            if same {
                self.owned.insert(key, o);
            } else {
                out.push(put_back(&key, &o));
            }
        }
        out
    }

    fn note(&mut self, out: &mut Vec<String>, stage: &str, state: String) {
        if self.last.get(stage) == Some(&state) {
            return;
        }
        out.push(format!("{stage}: {state}"));
        self.last.insert(stage.to_owned(), state);
    }

    fn set_matched(&mut self, index: usize, st: &Stage, out: &mut Vec<String>) {
        let key = &st.key;
        let refusal = ffi::refusal(&self.spec, index);
        if !refusal.is_empty() {
            self.note(
                out,
                key,
                format!("refused by preflight, not set:\n{refusal}"),
            );
            return;
        }
        let (pid, tid) = match find_threads(&st.matcher, &self.roots)[..] {
            [] => return self.note(out, key, "no thread matches".into()),
            [one] => one,
            ref many => {
                let at: Vec<String> = many.iter().map(|&(p, t)| at(p, t)).collect();
                let state = format!(
                    "matches {} threads ({}); refusing to guess",
                    many.len(),
                    at.join(", ")
                );
                return self.note(out, key, state);
            }
        };
        let start = ffi::task_starttime(pid, tid);
        let known = self
            .owned
            .get(key)
            .is_some_and(|o| o.tid == tid && o.start == start);
        if !known {
            let original = match ffi::observe(tid) {
                Ok(s) => s,
                Err(e) => return self.note(out, key, format!("cannot read {}: {e}", at(pid, tid))),
            };
            let o = Owned {
                pid,
                tid,
                start,
                original,
                matcher: st.matcher.clone(),
            };
            self.owned.insert(key.clone(), o);
        }
        if ffi::drift(&self.spec, index, tid).is_empty() {
            return self.note(out, key, format!("set on {}", at(pid, tid)));
        }
        let state = match ffi::apply(&self.spec, index, tid) {
            Ok(()) => format!("set on {}", at(pid, tid)),
            Err(e) => format!("cannot set on {}: {e}", at(pid, tid)),
        };
        self.note(out, key, state);
    }

    fn watch_bound(&mut self, index: usize, st: &Stage, held: &[Binding], out: &mut Vec<String>) {
        let mut declared = st.cpus.clone();
        declared.sort_unstable();
        for b in held {
            if b.node != st.node || b.group != st.group || b.name != st.name {
                continue;
            }
            let drift = ffi::drift(&self.spec, index, b.tid);
            let state = if drift.is_empty() {
                "runs as declared".to_owned()
            } else {
                format!("drifted (the thread owns it; reported, not changed):\n{drift}")
            };
            self.note(out, &format!("{} {}", st.key, at(b.pid, b.tid)), state);

            if st.children_cpus.is_empty() {
                continue;
            }
            let tasks = self.roots.proc.join(b.pid.to_string()).join("task");
            for tid in numeric_entries(&tasks) {
                if held.iter().any(|h| h.tid == tid) {
                    continue;
                }
                let Ok(ts) = ffi::observe(tid) else {
                    continue;
                };
                // An unset child still carries the affinity it inherited: exactly the stage's cpus.
                if ts.policy != libc::SCHED_OTHER || ts.cpus != declared {
                    continue;
                }
                out.push(match ffi::move_to(tid, &st.children_cpus) {
                    Ok(()) => format!(
                        "{}: moved child {} off the stage's cpus",
                        st.key,
                        at(b.pid, tid)
                    ),
                    Err(e) => format!("{}: cannot move child {}: {e}", st.key, at(b.pid, tid)),
                });
            }
        }
    }
}

fn put_back(key: &str, o: &Owned) -> String {
    if ffi::task_starttime(o.pid, o.tid) != o.start {
        return format!("{key}: thread gone, nothing to put back");
    }
    match ffi::put_back(o.tid, &o.original) {
        Ok(()) => format!("{key}: put back on {}", at(o.pid, o.tid)),
        Err(e) => format!("{key}: cannot put back {}: {e}", at(o.pid, o.tid)),
    }
}
