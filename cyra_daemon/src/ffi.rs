//! The daemon's only way into cyra_core. The chain file, its rules and every verdict stay in the
//! core; the Rust side walks /proc, loops and keeps what it set.

#[cxx::bridge(namespace = "cyrad::bridge")]
mod bridge {
    #[derive(Clone, Debug, Default, PartialEq, Eq)]
    struct Match {
        irq: String,
        process: String,
        thread: String,
    }

    #[derive(Clone, Debug)]
    struct Stage {
        /// How messages spell it: `node`, `node/group`, or `name`.
        key: String,
        node: String,
        group: String,
        name: String,
        external: bool,
        matcher: Match,
        cpus: Vec<u32>,
        children_cpus: Vec<u32>,
    }

    /// `policy` is the raw SCHED_* value.
    #[derive(Clone, Debug, Default)]
    struct ThreadState {
        policy: i32,
        priority: i32,
        cpus: Vec<u32>,
        reset_on_fork: bool,
    }

    #[derive(Clone, Debug)]
    struct Binding {
        node: String,
        group: String,
        name: String,
        pid: i32,
        tid: i32,
    }

    /// A thread, told apart from a later one reusing its tid by its start time.
    #[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
    struct Task {
        tid: i32,
        start: u64,
    }

    /// A held stage and the threads its process already had when it was bound: never its
    /// children.
    #[derive(Clone, Debug)]
    struct HeldStage {
        binding: Binding,
        earlier: Vec<Task>,
    }

    unsafe extern "C++" {
        include!("bridge.hpp");

        #[namespace = "cyra"]
        type Spec;

        fn load(path: &str) -> Result<UniquePtr<Spec>>;
        fn stages(spec: &Spec) -> Vec<Stage>;
        /// Empty when this host lets stage `index` run as declared on thread `tid`; preflight's
        /// report when a Fail, or a Warn on a hard chain, refuses it. Memory is judged on the
        /// thread's own process.
        fn refusal(spec: &Spec, index: usize, tid: i32) -> String;
        /// Empty when thread `tid` runs as stage `index` declares; verify's report when not.
        fn drift(spec: &Spec, index: usize, tid: i32) -> String;
        /// Stage `index` on thread `tid`, judged and read back.
        fn apply(spec: &Spec, index: usize, tid: i32) -> Result<()>;
        fn observe(tid: i32) -> Result<ThreadState>;
        /// Thread `tid` back to `state`, as observe() read it before the daemon set it. Only
        /// SCHED_OTHER, SCHED_FIFO and SCHED_RR can be put back; anything else throws.
        fn put_back(tid: i32, state: &ThreadState) -> Result<()>;
        /// Thread `tid`'s affinity only.
        fn move_to(tid: i32, cpus: &[u32]) -> Result<()>;
        fn bindings() -> Vec<Binding>;
        fn held_stages() -> Vec<HeldStage>;
        /// The daemon's directory, checked like its record's.
        fn daemon_dir() -> Result<String>;
        /// Publish the daemon's record (CYRA_DAEMON_DIR) as holding `held`. Empty, or why it
        /// was not written.
        fn record(held: &[Binding]) -> String;
        fn task_starttime(pid: i32, tid: i32) -> u64;
        fn bind(target: &str) -> Result<()>;
        fn this_tid() -> i32;
    }
}

pub use bridge::*;

impl Match {
    pub fn is_empty(&self) -> bool {
        self.irq.is_empty() && self.process.is_empty() && self.thread.is_empty()
    }
}
