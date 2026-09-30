//! SIGHUP, SIGTERM and SIGINT, taken synchronously: blocked before the first pass and waited for
//! between passes, so a signal never lands inside one.

#![allow(unsafe_code)]

use std::io;
use std::time::Duration;

pub enum Signal {
    Reload,
    Stop,
}

pub struct Signals {
    set: libc::sigset_t,
}

impl Signals {
    /// Block the three on the calling thread. Call it before starting any thread: a thread started
    /// earlier keeps the default handling and would be killed by SIGTERM instead.
    pub fn block() -> io::Result<Self> {
        // SAFETY: sigset_t is plain data, so all zeroes is a value; sigemptyset then defines it.
        let mut set: libc::sigset_t = unsafe { std::mem::zeroed() };
        // SAFETY: `set` is a live sigset_t, and the call writes only it.
        unsafe { libc::sigemptyset(&raw mut set) };
        for sig in [libc::SIGHUP, libc::SIGTERM, libc::SIGINT] {
            // SAFETY: as above, with a valid signal number.
            unsafe { libc::sigaddset(&raw mut set, sig) };
        }
        // SAFETY: `set` is initialised and outlives the call; a null old mask is allowed.
        let rc =
            unsafe { libc::pthread_sigmask(libc::SIG_BLOCK, &raw const set, std::ptr::null_mut()) };
        if rc != 0 {
            return Err(io::Error::from_raw_os_error(rc));
        }
        Ok(Self { set })
    }

    /// The next of the three within `timeout`, or None when the timeout comes first.
    pub fn wait(&self, timeout: Duration) -> io::Result<Option<Signal>> {
        let ts = libc::timespec {
            tv_sec: timeout.as_secs() as libc::time_t,
            tv_nsec: libc::c_long::from(timeout.subsec_nanos()),
        };
        // SAFETY: both pointers are to live values of the right type; a null siginfo is allowed.
        let sig =
            unsafe { libc::sigtimedwait(&raw const self.set, std::ptr::null_mut(), &raw const ts) };
        match sig {
            libc::SIGHUP => Ok(Some(Signal::Reload)),
            libc::SIGTERM | libc::SIGINT => Ok(Some(Signal::Stop)),
            _ => {
                let e = io::Error::last_os_error();
                match e.raw_os_error() {
                    Some(libc::EAGAIN | libc::EINTR) => Ok(None),
                    _ => Err(e),
                }
            }
        }
    }
}
