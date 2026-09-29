// The daemon against real threads. The targets are threads of this test process, named the way a
// library names its threads and matched by that name. Affinity needs no privilege, so these hold
// on any host; the SCHED_FIFO case runs where RLIMIT_RTPRIO allows it.
//
// The tests share this process's one record and run one at a time (serial()).

use std::env;
use std::fs;
use std::io::{BufRead, BufReader};
use std::path::PathBuf;
use std::process::{Command, Stdio};
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::{Arc, Mutex, MutexGuard, mpsc};
use std::thread::{self, JoinHandle};
use std::time::Duration;

use cyrad::ffi::{self, Match};
use cyrad::reconcile::{Reconciler, Roots, find_threads};

fn serial() -> MutexGuard<'static, ()> {
    static LOCK: Mutex<()> = Mutex::new(());
    LOCK.lock().unwrap_or_else(|e| e.into_inner())
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
            .join(format!("cyrad_test_{}_{n}.yaml", std::process::id()));
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
        let stop = Arc::new(AtomicBool::new(false));
        let (tx, rx) = mpsc::channel();
        let flag = Arc::clone(&stop);
        let handle = thread::Builder::new()
            .name(name.to_owned())
            .spawn(move || {
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
    let target = Target::new("cyrad_t_one");
    let f = SpecFile::new(&match_spec("cyrad_t_one", *all.last().unwrap()));
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
        "cyra status would not see the stage the daemon set"
    );
    rec.restore();
}

#[test]
fn refuses_to_guess_between_two_threads() {
    let _serial = serial();
    let all = cpus();
    let a = Target::new("cyrad_t_twin");
    let _b = Target::new("cyrad_t_twin");
    let before = ffi::observe(a.tid).unwrap().cpus;
    let f = SpecFile::new(&match_spec("cyrad_t_twin", *all.last().unwrap()));
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
        eprintln!("skipped: cyra-cap:two-cpus needs a cpu to drift to");
        return;
    }
    let last = *all.last().unwrap();
    let target = Target::new("cyrad_t_drift");
    let original = ffi::observe(target.tid).unwrap().cpus;
    let f = SpecFile::new(&match_spec("cyrad_t_drift", last));
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
    let _kept = Target::new("cyrad_t_keep");
    let dropped = Target::new("cyrad_t_drop");
    let original = ffi::observe(dropped.tid).unwrap().cpus;
    let comm = self_comm();
    let stage = |name: &str| {
        format!(
            "      - {{name: {name}, match: {{process: {comm}, thread: cyrad_t_{name}}}, \
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
    let target = Target::new("cyrad_t_deny");
    let f = SpecFile::new(&format!(
        "chains:\n  c:\n    target: soft\n    stages:\n      - name: tgt\n        \
         match: {{process: {}, thread: cyrad_t_deny}}\n        policy: fifo\n        \
         priority: 99\n",
        self_comm()
    ));
    if ffi::refusal(&ffi::load(&f.path).unwrap(), 0).is_empty() {
        eprintln!("skipped: cyra-cap:rtprio this host grants priority 99");
        return;
    }
    let mut rec = Reconciler::load(&f.path).unwrap();
    let lines = rec.pass();
    assert!(contains(&lines, "refused by preflight"), "{lines:?}");
    assert_ne!(ffi::observe(target.tid).unwrap().policy, libc::SCHED_FIFO);
}

const HOLDER: &str = "CYRAD_TEST_HOLDER";

/// Not a test of its own: the process moves_reset_children_off_the_stages_cpus starts to hold a
/// stage the way a node that binds does. Returns at once unless that test started it.
#[test]
fn holder() {
    if env::var_os(HOLDER).is_none() {
        return;
    }
    ffi::bind("/holder").unwrap();
    let holder = ffi::this_tid();
    let (tx, rx) = mpsc::channel();
    thread::spawn(move || {
        tx.send(ffi::this_tid()).unwrap();
        loop {
            thread::park();
        }
    });
    println!("tids {holder} {}", rx.recv().unwrap());
    loop {
        thread::park();
    }
}

// A stage a thread binds for itself is the thread's; the daemon moves its reset children off its
// cpus and leaves the holder alone.
#[test]
fn moves_reset_children_off_the_stages_cpus() {
    let _serial = serial();
    let all = cpus();
    if all.len() < 2 {
        eprintln!("skipped: cyra-cap:two-cpus needs a cpu for the children");
        return;
    }
    let (first, last) = (all[0], *all.last().unwrap());
    let f = SpecFile::new(&format!(
        "chains:\n  c:\n    target: soft\n    stages:\n      - node: /holder\n        \
         cpus: [{first}]\n        children_cpus: [{last}]\n"
    ));
    let mut proc = Command::new(env::current_exe().unwrap())
        .args(["holder", "--exact", "--nocapture"])
        .env(HOLDER, "1")
        .env("CYRA_SPEC", &f.path)
        .stdout(Stdio::piped())
        .spawn()
        .unwrap();
    let line = BufReader::new(proc.stdout.take().unwrap())
        .lines()
        .map_while(Result::ok)
        .find(|l| l.starts_with("tids "))
        .expect("the holder process did not report its threads");
    let tids: Vec<i32> = line[5..].split(' ').map(|t| t.parse().unwrap()).collect();

    let mut rec = Reconciler::load(&f.path).unwrap();
    let lines = rec.pass();
    let child_cpus = ffi::observe(tids[1]).unwrap().cpus;
    let holder_cpus = ffi::observe(tids[0]).unwrap().cpus;
    proc.kill().unwrap();
    proc.wait().unwrap();

    assert!(contains(&lines, "moved child"), "{lines:?}");
    assert_eq!(child_cpus, [last]);
    assert_eq!(holder_cpus, [first], "the holder is not the daemon's");
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
