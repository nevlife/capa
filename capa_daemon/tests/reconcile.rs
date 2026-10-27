// The daemon against real threads. The targets are threads of this test process, named the way a
// library names its threads and matched by that name. Affinity needs no privilege, so these hold
// on any host; the SCHED_FIFO case runs where RLIMIT_RTPRIO allows it.
//
// The tests share this process's one record and run one at a time (serial()).

use std::env;
use std::fs;
use std::io::{BufRead, BufReader};
use std::path::PathBuf;
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::{Arc, Mutex, MutexGuard, mpsc};
use std::thread::{self, JoinHandle};
use std::time::Duration;

use capad::ffi::{self, Match};
use capad::reconcile::{Reconciler, Roots, find_threads};

fn serial() -> MutexGuard<'static, ()> {
    static LOCK: Mutex<()> = Mutex::new(());
    LOCK.lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
}

fn cpus() -> Vec<u32> {
    ffi::observe(ffi::this_tid()).unwrap().cpus
}

fn self_comm() -> String {
    fs::read_to_string("/proc/self/comm")
        .unwrap()
        .trim_end()
        .to_owned()
}

struct SpecFile {
    path: String,
}

impl SpecFile {
    fn new(body: &str) -> Self {
        static N: AtomicU32 = AtomicU32::new(0);
        let n = N.fetch_add(1, Ordering::Relaxed);
        let path = PathBuf::from(env!("CARGO_TARGET_TMPDIR"))
            .join(format!("capad_test_{}_{n}.yaml", std::process::id()));
        fs::write(&path, body).unwrap();
        Self {
            path: path.to_str().unwrap().to_owned(),
        }
    }
}

impl Drop for SpecFile {
    fn drop(&mut self) {
        let _ = fs::remove_file(&self.path);
    }
}

fn match_spec(thread: &str, cpu: u32) -> String {
    format!(
        "chains:\n  c:\n    target: soft\n    stages:\n      - name: tgt\n        \
         match: {{process: {}, thread: {thread}}}\n        cpus: [{cpu}]\n",
        self_comm()
    )
}

/// A parked thread of this process under `name`.
struct Target {
    tid: i32,
    stop: Arc<AtomicBool>,
    handle: Option<JoinHandle<()>>,
}

impl Target {
    fn new(name: &str) -> Self {
        Self::with(name, || {})
    }

    /// The same, running `setup` on itself before it reports.
    fn with(name: &str, setup: fn()) -> Self {
        let stop = Arc::new(AtomicBool::new(false));
        let (tx, rx) = mpsc::channel();
        let flag = Arc::clone(&stop);
        let handle = thread::Builder::new()
            .name(name.to_owned())
            .spawn(move || {
                setup();
                tx.send(ffi::this_tid()).unwrap();
                while !flag.load(Ordering::Relaxed) {
                    thread::sleep(Duration::from_millis(1));
                }
            })
            .unwrap();
        Self {
            tid: rx.recv().unwrap(),
            stop,
            handle: Some(handle),
        }
    }

    fn rename(&self, name: &str) {
        fs::write(format!("/proc/self/task/{}/comm", self.tid), name).unwrap();
    }
}

impl Drop for Target {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Relaxed);
        if let Some(h) = self.handle.take() {
            h.join().unwrap();
        }
    }
}

fn contains(lines: &[String], needle: &str) -> bool {
    lines.iter().any(|l| l.contains(needle))
}

#[test]
fn sets_the_one_thread_a_stage_matches() {
    let _serial = serial();
    let all = cpus();
    let target = Target::new("capad_t_one");
    let f = SpecFile::new(&match_spec("capad_t_one", *all.last().unwrap()));
    let mut rec = Reconciler::load(&f.path).unwrap();

    let lines = rec.pass();
    assert!(contains(&lines, "tgt: set on pid"), "{lines:?}");
    assert_eq!(
        ffi::observe(target.tid).unwrap().cpus,
        [*all.last().unwrap()]
    );
    assert!(
        ffi::bindings()
            .iter()
            .any(|b| b.name == "tgt" && b.tid == target.tid),
        "capa status would not see the stage the daemon set"
    );
    rec.restore();
}

#[test]
fn refuses_to_guess_between_two_threads() {
    let _serial = serial();
    let all = cpus();
    let a = Target::new("capad_t_twin");
    let _b = Target::new("capad_t_twin");
    let before = ffi::observe(a.tid).unwrap().cpus;
    let f = SpecFile::new(&match_spec("capad_t_twin", *all.last().unwrap()));
    let mut rec = Reconciler::load(&f.path).unwrap();

    let lines = rec.pass();
    assert!(contains(&lines, "matches 2 threads"), "{lines:?}");
    assert_eq!(
        ffi::observe(a.tid).unwrap().cpus,
        before,
        "one of two was set anyway"
    );
}

#[test]
fn puts_a_drifted_thread_back_and_restores_on_stop() {
    let _serial = serial();
    let all = cpus();
    if all.len() < 2 {
        eprintln!("skipped: capa-cap:two-cpus needs a cpu to drift to");
        return;
    }
    let last = *all.last().unwrap();
    let target = Target::new("capad_t_drift");
    let original = ffi::observe(target.tid).unwrap().cpus;
    let f = SpecFile::new(&match_spec("capad_t_drift", last));
    let mut rec = Reconciler::load(&f.path).unwrap();
    rec.pass();

    ffi::move_to(target.tid, &[all[0]]).unwrap(); // somebody else moves it
    rec.pass();
    assert_eq!(
        ffi::observe(target.tid).unwrap().cpus,
        [last],
        "the daemon owns a matched stage and must put it back"
    );

    let lines = rec.restore();
    assert!(contains(&lines, "put back"), "{lines:?}");
    assert_eq!(ffi::observe(target.tid).unwrap().cpus, original);
}

#[test]
fn a_reload_keeps_what_still_matches_and_puts_back_the_rest() {
    let _serial = serial();
    let last = *cpus().last().unwrap();
    let _kept = Target::new("capad_t_keep");
    let dropped = Target::new("capad_t_drop");
    let original = ffi::observe(dropped.tid).unwrap().cpus;
    let comm = self_comm();
    let stage = |name: &str| {
        format!(
            "      - {{name: {name}, match: {{process: {comm}, thread: capad_t_{name}}}, \
             cpus: [{last}]}}\n"
        )
    };
    let head = "chains:\n  c:\n    target: soft\n    stages:\n";
    let first = SpecFile::new(&format!("{head}{}{}", stage("keep"), stage("drop")));
    let mut older = Reconciler::load(&first.path).unwrap();
    older.pass();

    let second = SpecFile::new(&format!("{head}{}", stage("keep")));
    let mut newer = Reconciler::load(&second.path).unwrap();
    let lines = newer.adopt(&mut older);
    assert!(contains(&lines, "drop: put back"), "{lines:?}");
    assert!(
        !contains(&lines, "keep:"),
        "a stage still in the file was dropped in between"
    );
    assert_eq!(ffi::observe(dropped.tid).unwrap().cpus, original);
    newer.restore();
}

#[test]
fn a_preflight_refusal_sets_nothing() {
    let _serial = serial();
    let target = Target::new("capad_t_deny");
    let f = SpecFile::new(&format!(
        "chains:\n  c:\n    target: soft\n    stages:\n      - name: tgt\n        \
         match: {{process: {}, thread: capad_t_deny}}\n        policy: fifo\n        \
         priority: 99\n",
        self_comm()
    ));
    if ffi::refusal(&ffi::load(&f.path).unwrap(), 0, target.tid).is_empty() {
        eprintln!("skipped: capa-cap:rtprio this host grants priority 99");
        return;
    }
    let mut rec = Reconciler::load(&f.path).unwrap();
    let lines = rec.pass();
    assert!(contains(&lines, "refused by preflight"), "{lines:?}");
    assert_ne!(ffi::observe(target.tid).unwrap().policy, libc::SCHED_FIFO);
}

// A matched stage is the daemon's only while it names exactly one thread. When the match goes
// ambiguous or moves to another thread, the thread set before goes back first.
#[test]
fn a_matched_thread_goes_back_when_the_match_is_lost() {
    let _serial = serial();
    let last = *cpus().last().unwrap();
    let a = Target::new("capad_t_lose");
    let original = ffi::observe(a.tid).unwrap().cpus;
    let f = SpecFile::new(&match_spec("capad_t_lose", last));
    let mut rec = Reconciler::load(&f.path).unwrap();
    rec.pass();
    assert_eq!(ffi::observe(a.tid).unwrap().cpus, [last]);

    let b = Target::new("capad_t_lose");
    let lines = rec.pass();
    assert!(contains(&lines, "refusing to guess"), "{lines:?}");
    assert_eq!(
        ffi::observe(a.tid).unwrap().cpus,
        original,
        "an ambiguous match left the thread set before on the stage's cpus"
    );

    a.rename("capad_t_gone");
    rec.pass();
    assert_eq!(ffi::observe(b.tid).unwrap().cpus, [last]);
    rec.restore();
    assert_eq!(ffi::observe(a.tid).unwrap().cpus, original);
    assert_eq!(ffi::observe(b.tid).unwrap().cpus, original);
}

#[test]
fn a_matched_thread_goes_back_when_the_match_moves_to_another() {
    let _serial = serial();
    let last = *cpus().last().unwrap();
    let a = Target::new("capad_t_move");
    let original = ffi::observe(a.tid).unwrap().cpus;
    let f = SpecFile::new(&match_spec("capad_t_move", last));
    let mut rec = Reconciler::load(&f.path).unwrap();
    rec.pass();

    a.rename("capad_t_other");
    let b = Target::new("capad_t_move");
    let lines = rec.pass();
    assert!(contains(&lines, "put back"), "{lines:?}");
    assert_eq!(ffi::observe(a.tid).unwrap().cpus, original);
    assert_eq!(ffi::observe(b.tid).unwrap().cpus, [last]);
    rec.restore();
    assert_eq!(ffi::observe(b.tid).unwrap().cpus, original);
}

// capad restores only SCHED_OTHER, SCHED_FIFO and SCHED_RR, so it does not take a thread from any
// other policy.
#[test]
fn a_thread_capad_could_not_put_back_is_not_set() {
    #[allow(unsafe_code)]
    fn to_batch() {
        let p = libc::sched_param { sched_priority: 0 };
        // SAFETY: a syscall on the calling thread with a parameter that outlives it.
        let rc = unsafe { libc::sched_setscheduler(0, libc::SCHED_BATCH, &raw const p) };
        assert_eq!(rc, 0);
    }
    let _serial = serial();
    let t = Target::with("capad_t_batch", to_batch);
    let f = SpecFile::new(&match_spec("capad_t_batch", *cpus().last().unwrap()));
    let mut rec = Reconciler::load(&f.path).unwrap();
    let lines = rec.pass();
    assert!(contains(&lines, "cannot put back; not set"), "{lines:?}");
    assert_eq!(ffi::observe(t.tid).unwrap().policy, libc::SCHED_BATCH);
    assert!(rec.restore().is_empty());
}

const HOLDER: &str = "CAPAD_TEST_HOLDER";
const HOLDER_CPU: &str = "CAPAD_TEST_HOLDER_CPU";

/// Not a test of its own: a process the tests below start to hold /holder the way a node that
/// binds does. `child` starts a thread after the bind; `earlier` has a thread from before the
/// bind pin itself to the stage's cpu. Returns at once unless a test started it.
#[test]
fn holder() {
    let Ok(role) = env::var(HOLDER) else {
        return;
    };
    let (tx, rx) = mpsc::channel();
    let park = move || {
        tx.send(ffi::this_tid()).unwrap();
        loop {
            thread::park();
        }
    };
    let other = if role == "earlier" {
        let cpu: u32 = env::var(HOLDER_CPU).unwrap().parse().unwrap();
        thread::spawn(move || {
            ffi::move_to(ffi::this_tid(), &[cpu]).unwrap();
            park();
        });
        let other = rx.recv().unwrap();
        ffi::bind("/holder").unwrap();
        other
    } else {
        ffi::bind("/holder").unwrap();
        thread::spawn(park);
        rx.recv().unwrap()
    };
    println!("tids {} {other}", ffi::this_tid());
    loop {
        thread::park();
    }
}

struct Holder {
    proc: Child,
    bound: i32,
    other: i32,
}

impl Holder {
    fn start(role: &str, spec: &SpecFile, cpu: u32) -> Self {
        let mut proc = Command::new(env::current_exe().unwrap())
            .args(["holder", "--exact", "--nocapture"])
            .env(HOLDER, role)
            .env(HOLDER_CPU, cpu.to_string())
            .env("CAPA_SPEC", &spec.path)
            .stdout(Stdio::piped())
            .spawn()
            .unwrap();
        let line = BufReader::new(proc.stdout.take().unwrap())
            .lines()
            .map_while(Result::ok)
            .find(|l| l.starts_with("tids "))
            .expect("the holder process did not report its threads");
        let tids: Vec<i32> = line[5..].split(' ').map(|t| t.parse().unwrap()).collect();
        Self {
            proc,
            bound: tids[0],
            other: tids[1],
        }
    }
}

impl Drop for Holder {
    fn drop(&mut self) {
        let _ = self.proc.kill();
        let _ = self.proc.wait();
    }
}

fn holder_spec(cpu: u32, children: u32) -> SpecFile {
    SpecFile::new(&format!(
        "chains:\n  c:\n    target: soft\n    stages:\n      - node: /holder\n        \
         cpus: [{cpu}]\n        children_cpus: [{children}]\n"
    ))
}

// A stage a thread binds for itself is the thread's; the daemon moves its reset children off its
// cpus and leaves the holder alone.
#[test]
fn moves_reset_children_off_the_stages_cpus() {
    let _serial = serial();
    let all = cpus();
    if all.len() < 3 {
        eprintln!("skipped: capa-cap:three-cpus needs cpus for the stage and its children");
        return;
    }
    let (first, last) = (all[0], *all.last().unwrap());
    let f = holder_spec(first, last);
    let h = Holder::start("child", &f, first);
    let mut rec = Reconciler::load(&f.path).unwrap();
    let lines = rec.pass();
    assert!(contains(&lines, "moved child"), "{lines:?}");
    assert_eq!(ffi::observe(h.other).unwrap().cpus, [last]);
    assert_eq!(
        ffi::observe(h.bound).unwrap().cpus,
        [first],
        "the holder is not the daemon's"
    );
}

// The kernel does not say which thread started which. A thread the process had before the bind is
// not a child of the stage, whatever cpus it chose for itself.
#[test]
fn children_cpus_leaves_threads_from_before_the_bind() {
    let _serial = serial();
    let all = cpus();
    if all.len() < 3 {
        eprintln!("skipped: capa-cap:three-cpus needs cpus for the stage and its children");
        return;
    }
    let (first, last) = (all[0], *all.last().unwrap());
    let f = holder_spec(first, last);
    let h = Holder::start("earlier", &f, first);
    let lines = Reconciler::load(&f.path).unwrap().pass();
    assert!(!contains(&lines, "moved child"), "{lines:?}");
    assert_eq!(ffi::observe(h.other).unwrap().cpus, [first]);
}

// What capad moved it remembers: a stop moves it back, and a reload with other children_cpus
// moves it there.
#[test]
fn moved_children_follow_a_reload_and_go_back_on_stop() {
    let _serial = serial();
    let all = cpus();
    if all.len() < 3 {
        eprintln!("skipped: capa-cap:three-cpus needs cpus for the stage and its children");
        return;
    }
    let (first, mid, last) = (all[0], all[1], *all.last().unwrap());
    let f = holder_spec(first, last);
    let h = Holder::start("child", &f, first);
    let mut older = Reconciler::load(&f.path).unwrap();
    older.pass();
    assert_eq!(ffi::observe(h.other).unwrap().cpus, [last]);

    let g = holder_spec(first, mid);
    let mut newer = Reconciler::load(&g.path).unwrap();
    newer.adopt(&mut older);
    let lines = newer.pass();
    assert!(contains(&lines, "moved child"), "{lines:?}");
    assert_eq!(ffi::observe(h.other).unwrap().cpus, [mid]);

    let lines = newer.restore();
    assert!(contains(&lines, "moved child"), "{lines:?}");
    assert_eq!(ffi::observe(h.other).unwrap().cpus, [first]);
}

// A capad killed without its stop leaves the state file behind. The next one takes over from it,
// so a stop puts the thread back where it started, not where the killed run left it.
#[test]
fn a_restart_after_a_crash_still_puts_back_the_original() {
    let _serial = serial();
    let last = *cpus().last().unwrap();
    let t = Target::new("capad_t_crash");
    let original = ffi::observe(t.tid).unwrap().cpus;
    let f = SpecFile::new(&match_spec("capad_t_crash", last));
    let mut crashed = Reconciler::load(&f.path).unwrap();
    crashed.pass();
    drop(crashed); // no restore: killed

    let mut next = Reconciler::load(&f.path).unwrap();
    next.recover();
    next.pass();
    assert_eq!(ffi::observe(t.tid).unwrap().cpus, [last]);
    let lines = next.restore();
    assert!(contains(&lines, "put back"), "{lines:?}");
    assert_eq!(ffi::observe(t.tid).unwrap().cpus, original);
}

// What the crashed run set for a stage the new file no longer has goes back at once.
#[test]
fn a_restart_puts_back_what_the_new_file_dropped() {
    let _serial = serial();
    let last = *cpus().last().unwrap();
    let t = Target::new("capad_t_left");
    let original = ffi::observe(t.tid).unwrap().cpus;
    let f = SpecFile::new(&match_spec("capad_t_left", last));
    Reconciler::load(&f.path).unwrap().pass();

    let g = SpecFile::new(&match_spec("capad_t_other", last));
    let lines = Reconciler::load(&g.path).unwrap().recover();
    assert!(contains(&lines, "tgt: put back"), "{lines:?}");
    assert_eq!(ffi::observe(t.tid).unwrap().cpus, original);
}

#[test]
fn a_restart_after_a_crash_still_moves_children_back() {
    let _serial = serial();
    let all = cpus();
    if all.len() < 3 {
        eprintln!("skipped: capa-cap:three-cpus needs cpus for the stage and its children");
        return;
    }
    let (first, last) = (all[0], *all.last().unwrap());
    let f = holder_spec(first, last);
    let h = Holder::start("child", &f, first);
    Reconciler::load(&f.path).unwrap().pass();
    assert_eq!(ffi::observe(h.other).unwrap().cpus, [last]);

    let mut next = Reconciler::load(&f.path).unwrap();
    next.recover();
    next.pass();
    let lines = next.restore();
    assert!(contains(&lines, "moved child"), "{lines:?}");
    assert_eq!(ffi::observe(h.other).unwrap().cpus, [first]);
}

// Interrupt threads are named irq/<n>-<handler>: whole on kernels since 5.17, cut to 15 bytes
// before. The handler name comes from /sys/kernel/irq/<n>/actions. A secondary thread
// irq/<n>-s-<handler> is not the handler's.
#[test]
fn finds_an_interrupt_thread_by_its_handler_name() {
    let root =
        PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join(format!("irq_{}", std::process::id()));
    let put = |rel: &str, text: &str| {
        let path = root.join(rel);
        fs::create_dir_all(path.parent().unwrap()).unwrap();
        fs::write(path, format!("{text}\n")).unwrap();
    };
    put("sys/190/actions", "3550000.usb");
    put("sys/191/actions", "i2c,3550000.usb");
    put("sys/77/actions", "nvme0q1");
    put("proc/4100/comm", "irq/190-3550000.usb");
    put("proc/4101/comm", "irq/191-3550000");
    put("proc/4102/comm", "irq/190-s-35500");
    put("proc/4103/comm", "irq/77-nvme0q1");

    let roots = Roots {
        proc: root.join("proc"),
        sys_irq: root.join("sys"),
    };
    let irq = |name: &str| Match {
        irq: name.to_owned(),
        ..Match::default()
    };
    assert_eq!(
        find_threads(&irq("3550000.usb"), &roots),
        [(4100, 4100), (4101, 4101)],
        "a shared line names the handler too, and the secondary thread is not it"
    );
    assert_eq!(find_threads(&irq("nvme0q1"), &roots), [(4103, 4103)]);
    fs::remove_dir_all(&root).unwrap();
}

// Every Autoware container's comm was "agnocast_compon", so a match by comm alone named them all.
// One whole argv element tells them apart; a longer element that merely contains it does not.
#[test]
fn finds_a_process_by_one_argument_element() {
    let root =
        PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join(format!("arg_{}", std::process::id()));
    let put = |rel: &str, bytes: &[u8]| {
        let path = root.join(rel);
        fs::create_dir_all(path.parent().unwrap()).unwrap();
        fs::write(path, bytes).unwrap();
    };
    for (pid, node) in [
        (500, "__node:=behavior_planning_container"),
        (600, "__node:=motion_planning_container"),
        (700, "x__node:=behavior_planning_container"),
    ] {
        put(&format!("proc/{pid}/comm"), b"agnocast_compon\n");
        let cmdline = format!("/opt/component_container\0--ros-args\0-r\0{node}\0");
        put(&format!("proc/{pid}/cmdline"), cmdline.as_bytes());
        put(&format!("proc/{pid}/task/{pid}/comm"), b"agnocast_compon\n");
        put(&format!("proc/{pid}/task/{}/comm", pid + 1), b"recvUC\n");
    }
    let roots = Roots {
        proc: root.join("proc"),
        sys_irq: root.join("sys"),
    };
    let by = |process: &str, arg: &str| Match {
        process: process.to_owned(),
        arg: arg.to_owned(),
        thread: "recvUC".to_owned(),
        ..Match::default()
    };
    assert_eq!(
        find_threads(&by("agnocast_compon", ""), &roots).len(),
        3,
        "comm alone names every container"
    );
    let planning = "__node:=behavior_planning_container";
    assert_eq!(find_threads(&by("", planning), &roots), [(500, 501)]);
    assert_eq!(
        find_threads(&by("agnocast_compon", planning), &roots),
        [(500, 501)]
    );
    assert!(find_threads(&by("other", planning), &roots).is_empty());
    fs::remove_dir_all(&root).unwrap();
}

#[test]
fn sets_a_thread_found_by_an_argument_of_its_process() {
    let _serial = serial();
    let last = *cpus().last().unwrap();
    let argv0 = env::args().next().unwrap();
    let t = Target::new("capad_t_arg");
    let f = SpecFile::new(&format!(
        "chains:\n  c:\n    target: soft\n    stages:\n      - name: tgt\n        \
         match: {{arg: \"{argv0}\", thread: capad_t_arg}}\n        cpus: [{last}]\n"
    ));
    let mut rec = Reconciler::load(&f.path).unwrap();
    let lines = rec.pass();
    assert!(contains(&lines, "tgt: set on pid"), "{lines:?}");
    assert_eq!(ffi::observe(t.tid).unwrap().cpus, [last]);
    rec.restore();
}

// A capad updated while its previous run had crashed reads that run's state file, written before
// owned lines carried `arg`.
#[test]
fn a_restart_reads_the_state_file_of_the_previous_format() {
    let _serial = serial();
    let last = *cpus().last().unwrap();
    let t = Target::new("capad_t_v1");
    let original = ffi::observe(t.tid).unwrap();
    let pid = i32::try_from(std::process::id()).unwrap();
    let start = ffi::task_starttime(pid, t.tid);
    let cpus_text: Vec<String> = original.cpus.iter().map(u32::to_string).collect();
    let v1 = format!(
        "capad-state 1\nowned\ttgt\t{pid}\t{}\t{start}\t{}\t{}\t{}\t{}\t\t{}\tcapad_t_v1\n",
        t.tid,
        original.policy,
        original.priority,
        cpus_text.join(","),
        u8::from(original.reset_on_fork),
        self_comm()
    );
    fs::write(
        PathBuf::from(ffi::daemon_dir().unwrap()).join("capad.state"),
        v1,
    )
    .unwrap();
    ffi::move_to(t.tid, &[last]).unwrap();

    let f = SpecFile::new(&match_spec("capad_t_v1", last));
    let mut next = Reconciler::load(&f.path).unwrap();
    let lines = next.recover();
    assert!(!contains(&lines, "not understood"), "{lines:?}");
    assert!(!contains(&lines, "not a capad state file"), "{lines:?}");
    next.pass();
    let lines = next.restore();
    assert!(contains(&lines, "put back"), "{lines:?}");
    assert_eq!(ffi::observe(t.tid).unwrap().cpus, original.cpus);
}
