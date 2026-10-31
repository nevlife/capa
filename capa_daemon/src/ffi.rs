//! The daemon's only way into `capa_core`: the chain file, its rules and every verdict stay there.

#[cxx::bridge(namespace = "capad::bridge")]
mod bridge {
    #[derive(Clone, Debug, Default, PartialEq, Eq)]
    struct Match {
        irq: String,
        process: String,
        arg: String,
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

    /// `earlier`: the threads its process had when the stage was bound, never its children.
    /// `ask`: the thread may not set itself and asks capad to set it from the chain file.
    #[derive(Clone, Debug)]
    struct HeldStage {
        binding: Binding,
        earlier: Vec<Task>,
        ask: bool,
    }

    unsafe extern "C++" {
        include!("bridge.hpp");

        #[namespace = "capa"]
        type Spec;

        fn load(path: &str) -> Result<UniquePtr<Spec>>;
        fn stages(spec: &Spec) -> Vec<Stage>;
        /// Empty, or preflight's report refusing stage `index` on thread `tid`. Memory is judged
        /// on the thread's own process.
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
        /// Publish the daemon's record as holding `held`. Empty, or why it was not written.
        fn record(held: &[Binding]) -> String;
        fn task_starttime(pid: i32, tid: i32) -> u64;
        fn bind(target: &str) -> Result<()>;
        fn this_tid() -> i32;
    }
}

pub use bridge::*;

impl Match {
    pub fn is_empty(&self) -> bool {
        self.irq.is_empty()
            && self.process.is_empty()
            && self.arg.is_empty()
            && self.thread.is_empty()
    }
}
