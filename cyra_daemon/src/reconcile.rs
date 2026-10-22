//! One pass of the daemon over a chain file: set the threads it matches, put a drifted one back,
//! move reset children off their stage's cpus, report drift on the stages threads bind themselves,
//! and record what it holds so `cyra status` sees it. Everything the daemon decides beyond the
//! core's verdicts is here; main only loops, reloads and stops.

use std::collections::BTreeMap;
use std::fmt::Write as _;
use std::fs;
use std::path::{Path, PathBuf};

use cxx::UniquePtr;

use crate::ffi::{self, Binding, HeldStage, Match, Spec, Stage, Task, ThreadState};
use crate::state;

const STATE_HEAD: &str = "cyrad-state 1";

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

/// A reset child the daemon moved off its stage's cpus, and where it ran before.
struct Moved {
    stage: String,
    pid: i32,
    original: Vec<u32>,
}

pub struct Reconciler {
    spec: UniquePtr<Spec>,
    stages: Vec<Stage>,
    roots: Roots,
    owned: BTreeMap<String, Owned>,
    moved: BTreeMap<Task, Moved>,
    last: BTreeMap<String, String>,
    saved: String,
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
            moved: BTreeMap::new(),
            last: BTreeMap::new(),
            saved: String::new(),
        })
    }

    /// One pass. Returns what changed since the last pass, one line each.
    pub fn pass(&mut self) -> Vec<String> {
        let mut out = Vec::new();
        let held = ffi::held_stages();
        self.moved
            .retain(|task, m| ffi::task_starttime(m.pid, task.tid) == task.start);
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
        self.save(&mut out);
        out
    }

    /// Put every thread this daemon set or moved back to what it ran at before, and drop its
    /// record.
    pub fn restore(&mut self) -> Vec<String> {
        let owned = std::mem::take(&mut self.owned);
        let mut out: Vec<String> = owned.iter().map(|(key, o)| put_back(key, o)).collect();
        for (task, m) in std::mem::take(&mut self.moved) {
            out.push(move_back(task, &m));
        }
        ffi::record(&[]);
        self.save(&mut out);
        out
    }

    /// Take over what a crashed run left in the state file, as a reload takes over from the old
    /// file. Call once, before the first pass.
    pub fn recover(&mut self) -> Vec<String> {
        let text = match state::read() {
            Ok(Some(t)) => t,
            Ok(None) => return Vec::new(),
            Err(e) => return vec![format!("state: not read ({e})")],
        };
        let mut lines = text.lines();
        if lines.next() != Some(STATE_HEAD) {
            return vec!["state: not a cyrad state file; ignored".to_owned()];
        }
        let mut out = Vec::new();
        let mut owned = BTreeMap::new();
        let mut moved = BTreeMap::new();
        for line in lines {
            let fields: Vec<&str> = line.split('\t').collect();
            let parsed = match fields[..] {
                ["owned", ..] => parse_owned(&fields)
                    .map(|(k, o)| owned.insert(k, o))
                    .is_some(),
                ["moved", ..] => parse_moved(&fields)
                    .map(|(t, m)| moved.insert(t, m))
                    .is_some(),
                _ => false,
            };
            if !parsed {
                out.push(format!("state: line not understood, skipped: {line}"));
            }
        }
        out.extend(self.take_over(owned, moved));
        out
    }

    /// Take over what `older` set for stages this file still declares the same way; put the rest
    /// back. How a reload avoids dropping every thread to its default in between.
    pub fn adopt(&mut self, older: &mut Reconciler) -> Vec<String> {
        let owned = std::mem::take(&mut older.owned);
        let moved = std::mem::take(&mut older.moved);
        self.take_over(owned, moved)
    }

    fn take_over(
        &mut self,
        owned: BTreeMap<String, Owned>,
        moved: BTreeMap<Task, Moved>,
    ) -> Vec<String> {
        let mut out = Vec::new();
        for (key, o) in owned {
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
        for (task, m) in moved {
            let kept = self
                .stages
                .iter()
                .any(|s| s.key == m.stage && !s.children_cpus.is_empty());
            if kept {
                self.moved.insert(task, m);
            } else {
                out.push(move_back(task, &m));
            }
        }
        out
    }

    fn state_text(&self) -> String {
        let mut t = format!("{STATE_HEAD}\n");
        for (key, o) in &self.owned {
            let (s, m) = (&o.original, &o.matcher);
            writeln!(
                t,
                "owned\t{key}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}",
                o.pid,
                o.tid,
                o.start,
                s.policy,
                s.priority,
                join_cpus(&s.cpus),
                u8::from(s.reset_on_fork),
                m.irq,
                m.process,
                m.thread
            )
            .expect("a String takes any write");
        }
        for (task, m) in &self.moved {
            writeln!(
                t,
                "moved\t{}\t{}\t{}\t{}\t{}",
                m.stage,
                m.pid,
                task.tid,
                task.start,
                join_cpus(&m.original)
            )
            .expect("a String takes any write");
        }
        t
    }

    fn save(&mut self, out: &mut Vec<String>) {
        let text = self.state_text();
        if text == self.saved {
            return;
        }
        match state::write(&text) {
            Ok(()) => self.saved = text,
            Err(e) => {
                let state = format!("not saved ({e}); a restart could not put back what it set");
                self.note(out, "state", state);
            }
        }
    }

    fn note(&mut self, out: &mut Vec<String>, stage: &str, state: String) {
        if self.last.get(stage) == Some(&state) {
            return;
        }
        out.push(format!("{stage}: {state}"));
        self.last.insert(stage.to_owned(), state);
    }

    fn release(&mut self, key: &str, out: &mut Vec<String>) {
        if let Some(o) = self.owned.remove(key) {
            out.push(put_back(key, &o));
        }
    }

    fn set_matched(&mut self, index: usize, st: &Stage, out: &mut Vec<String>) {
        let key = &st.key;
        let (pid, tid) = match find_threads(&st.matcher, &self.roots)[..] {
            [] => {
                self.release(key, out);
                return self.note(out, key, "no thread matches".into());
            }
            [one] => one,
            ref many => {
                let at: Vec<String> = many.iter().map(|&(p, t)| at(p, t)).collect();
                let state = format!(
                    "matches {} threads ({}); refusing to guess",
                    many.len(),
                    at.join(", ")
                );
                self.release(key, out);
                return self.note(out, key, state);
            }
        };
        let start = ffi::task_starttime(pid, tid);
        if self
            .owned
            .get(key)
            .is_some_and(|o| o.tid != tid || o.start != start)
        {
            self.release(key, out);
        }
        let refusal = ffi::refusal(&self.spec, index, tid);
        if !refusal.is_empty() {
            self.release(key, out);
            let state = format!("refused by preflight, not set:\n{refusal}");
            return self.note(out, key, state);
        }
        let fresh = !self.owned.contains_key(key);
        if fresh {
            let original = match ffi::observe(tid) {
                Ok(s) => s,
                Err(e) => return self.note(out, key, format!("cannot read {}: {e}", at(pid, tid))),
            };
            // Only these can be put back; a thread taken from BATCH, IDLE or DEADLINE could not.
            if ![libc::SCHED_OTHER, libc::SCHED_FIFO, libc::SCHED_RR].contains(&original.policy) {
                let state = format!(
                    "{} runs policy {}, which cyrad cannot put back; not set",
                    at(pid, tid),
                    original.policy
                );
                return self.note(out, key, state);
            }
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
            Ok(()) if ffi::task_starttime(pid, tid) != start => {
                self.owned.remove(key);
                format!(
                    "tid {tid} ended while being set; whatever reuses it may now carry the stage"
                )
            }
            Ok(()) => format!("set on {}", at(pid, tid)),
            Err(e) => {
                // apply undoes a refused syscall, so a thread taken this pass is left as it was.
                if fresh {
                    self.owned.remove(key);
                }
                format!("cannot set on {}: {e}", at(pid, tid))
            }
        };
        self.note(out, key, state);
    }

    fn watch_bound(&mut self, index: usize, st: &Stage, held: &[HeldStage], out: &mut Vec<String>) {
        let mut declared = st.cpus.clone();
        declared.sort_unstable();
        let mut wanted = st.children_cpus.clone();
        wanted.sort_unstable();
        for h in held {
            let b = &h.binding;
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
                let start = ffi::task_starttime(b.pid, tid);
                let task = Task { tid, start };
                if start == 0 || h.earlier.contains(&task) {
                    continue;
                }
                if held.iter().any(|x| x.binding.tid == tid) {
                    self.moved.remove(&task); // it holds a stage of its own now
                    continue;
                }
                let Ok(ts) = ffi::observe(tid) else {
                    continue;
                };
                if self.moved.get(&task).is_some_and(|m| m.stage == st.key) {
                    // Moved before: it follows children_cpus through a reload, and back from a
                    // move by someone else.
                    if ts.cpus == wanted {
                        continue;
                    }
                } else if ts.policy != libc::SCHED_OTHER || ts.cpus != declared {
                    // A reset child still carries the affinity it inherited: the stage's cpus.
                    continue;
                }
                let line = match ffi::move_to(tid, &st.children_cpus) {
                    Ok(()) if ffi::task_starttime(b.pid, tid) != start => {
                        format!("{}: tid {tid} ended while being moved", st.key)
                    }
                    Ok(()) => {
                        self.moved.entry(task).or_insert(Moved {
                            stage: st.key.clone(),
                            pid: b.pid,
                            original: ts.cpus.clone(),
                        });
                        format!("{}: moved child {} to {wanted:?}", st.key, at(b.pid, tid))
                    }
                    Err(e) => format!("{}: cannot move child {}: {e}", st.key, at(b.pid, tid)),
                };
                out.push(line);
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

fn move_back(task: Task, m: &Moved) -> String {
    let child = at(m.pid, task.tid);
    if ffi::task_starttime(m.pid, task.tid) != task.start {
        return format!("{}: child {child} gone, nothing to move back", m.stage);
    }
    match ffi::move_to(task.tid, &m.original) {
        Ok(()) => format!("{}: moved child {child} back to {:?}", m.stage, m.original),
        Err(e) => format!("{}: cannot move child {child} back: {e}", m.stage),
    }
}

fn join_cpus(cpus: &[u32]) -> String {
    cpus.iter()
        .map(u32::to_string)
        .collect::<Vec<_>>()
        .join(",")
}

fn parse_cpus(field: &str) -> Option<Vec<u32>> {
    if field.is_empty() {
        return Some(Vec::new());
    }
    field.split(',').map(|c| c.parse().ok()).collect()
}

fn parse_owned(f: &[&str]) -> Option<(String, Owned)> {
    let [
        _,
        key,
        pid,
        tid,
        start,
        policy,
        priority,
        cpus,
        reset,
        irq,
        process,
        thread,
    ] = f
    else {
        return None;
    };
    let original = ThreadState {
        policy: policy.parse().ok()?,
        priority: priority.parse().ok()?,
        cpus: parse_cpus(cpus)?,
        reset_on_fork: *reset == "1",
    };
    let matcher = Match {
        irq: (*irq).to_owned(),
        process: (*process).to_owned(),
        thread: (*thread).to_owned(),
    };
    let o = Owned {
        pid: pid.parse().ok()?,
        tid: tid.parse().ok()?,
        start: start.parse().ok()?,
        original,
        matcher,
    };
    Some(((*key).to_owned(), o))
}

fn parse_moved(f: &[&str]) -> Option<(Task, Moved)> {
    let [_, stage, pid, tid, start, cpus] = f else {
        return None;
    };
    let task = Task {
        tid: tid.parse().ok()?,
        start: start.parse().ok()?,
    };
    let m = Moved {
        stage: (*stage).to_owned(),
        pid: pid.parse().ok()?,
        original: parse_cpus(cpus)?,
    };
    Some((task, m))
}
