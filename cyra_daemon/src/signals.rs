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
        // SAFETY: sigset_t is plain data; sigemptyset initialises it and the calls only touch it
        // and the calling thread's mask.
        unsafe {
            let mut set: libc::sigset_t = std::mem::zeroed();
            libc::sigemptyset(&mut set);
            for sig in [libc::SIGHUP, libc::SIGTERM, libc::SIGINT] {
                libc::sigaddset(&mut set, sig);
            }
            let rc = libc::pthread_sigmask(libc::SIG_BLOCK, &set, std::ptr::null_mut());
            if rc != 0 {
                return Err(io::Error::from_raw_os_error(rc));
            }
            Ok(Self { set })
        }
    }

    /// The next of the three within `timeout`, or None when the timeout comes first.
    pub fn wait(&self, timeout: Duration) -> io::Result<Option<Signal>> {
        let ts = libc::timespec {
            tv_sec: timeout.as_secs() as libc::time_t,
            tv_nsec: libc::c_long::from(timeout.subsec_nanos()),
        };
        // SAFETY: both pointers are to live values of the right type; a null siginfo is allowed.
        let sig = unsafe { libc::sigtimedwait(&self.set, std::ptr::null_mut(), &ts) };
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
