//! The kernel's process events (proc connector, netlink): the daemon reads them between passes to
//! know whether a pass has to walk /proc. They only say when to look; what is true is still read
//! from /proc, so a forged or wrong event costs one walk and sets nothing.

#![allow(unsafe_code)]

use std::io;
use std::mem::{size_of, zeroed};
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd};

const NETLINK_CONNECTOR: libc::c_int = 11;
const NETLINK_FAMILY: libc::sa_family_t = 16;
const CN_IDX_PROC: u32 = 1;
const CN_VAL_PROC: u32 = 1;
const PROC_CN_MCAST_LISTEN: u32 = 1;
const NLMSG_DONE: u16 = 3;

const NLMSG_HDR: usize = 16;
const CN_MSG: usize = 20;
const EVENT_HEAD: usize = NLMSG_HDR + CN_MSG + 16;

const PROC_EVENT_NONE: u32 = 0;
const PROC_EVENT_FORK: u32 = 0x1;
const PROC_EVENT_EXEC: u32 = 0x2;
const PROC_EVENT_COMM: u32 = 0x200;
const PROC_EVENT_EXIT: u32 = 0x8000_0000;

const BUFFER: usize = 8192;
const ADDR_LEN: libc::socklen_t = 12;
const _: () = assert!(size_of::<libc::sockaddr_nl>() == ADDR_LEN as usize);
const LISTEN_LEN: u32 = 40;
const _: () = assert!(NLMSG_HDR + CN_MSG + 4 == LISTEN_LEN as usize);
const ACK_WAIT_MS: libc::c_int = 1000;

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Event {
    /// A new thread or process.
    Fork {
        pid: i32,
        tid: i32,
    },
    Exec {
        pid: i32,
    },
    /// A thread took the name `comm`.
    Comm {
        comm: Vec<u8>,
    },
    Exit {
        tid: i32,
    },
}

fn u32_at(buf: &[u8], at: usize) -> Option<u32> {
    let bytes = buf.get(at..at + 4)?;
    Some(u32::from_ne_bytes(bytes.try_into().ok()?))
}

fn i32_at(buf: &[u8], at: usize) -> Option<i32> {
    let bytes = buf.get(at..at + 4)?;
    Some(i32::from_ne_bytes(bytes.try_into().ok()?))
}

/// The event one netlink message carries, or None for what is not one (an ack, a short message,
/// another connector's traffic, an event the daemon has no use for).
pub fn parse(msg: &[u8]) -> Option<Event> {
    if u32_at(msg, NLMSG_HDR)? != CN_IDX_PROC || u32_at(msg, NLMSG_HDR + 4)? != CN_VAL_PROC {
        return None;
    }
    let what = u32_at(msg, NLMSG_HDR + CN_MSG)?;
    let data = EVENT_HEAD;
    match what {
        PROC_EVENT_FORK => Some(Event::Fork {
            tid: i32_at(msg, data + 8)?,
            pid: i32_at(msg, data + 12)?,
        }),
        PROC_EVENT_EXEC => Some(Event::Exec {
            pid: i32_at(msg, data + 4)?,
        }),
        PROC_EVENT_COMM => {
            let name = msg.get(data + 8..data + 24)?;
            let end = name.iter().position(|&b| b == 0).unwrap_or(name.len());
            Some(Event::Comm {
                comm: name[..end].to_vec(),
            })
        }
        PROC_EVENT_EXIT => Some(Event::Exit {
            tid: i32_at(msg, data)?,
        }),
        _ => None,
    }
}

/// The error an ack carries, or None when `msg` is no ack.
fn ack_error(msg: &[u8]) -> Option<u32> {
    if u32_at(msg, NLMSG_HDR + CN_MSG)? != PROC_EVENT_NONE {
        return None;
    }
    u32_at(msg, EVENT_HEAD)
}

pub struct ProcEvents {
    fd: OwnedFd,
}

impl ProcEvents {
    /// Join the process events and ask the kernel to send them. Fails on a kernel without them or
    /// one that wants `CAP_NET_ADMIN` for this; the daemon then walks /proc every pass.
    pub fn open() -> io::Result<Self> {
        // SAFETY: plain arguments; the return value is checked before use.
        let raw = unsafe {
            libc::socket(
                libc::AF_NETLINK,
                libc::SOCK_DGRAM | libc::SOCK_CLOEXEC | libc::SOCK_NONBLOCK,
                NETLINK_CONNECTOR,
            )
        };
        if raw < 0 {
            return Err(io::Error::last_os_error());
        }
        // SAFETY: `raw` is a socket just opened here and owned by nothing else.
        let fd = unsafe { OwnedFd::from_raw_fd(raw) };
        // SAFETY: sockaddr_nl is plain data, so all zeroes is a value.
        let mut addr: libc::sockaddr_nl = unsafe { zeroed() };
        addr.nl_family = NETLINK_FAMILY;
        addr.nl_groups = CN_IDX_PROC;
        // SAFETY: `addr` is a live sockaddr_nl and the length is its size.
        let rc = unsafe { libc::bind(fd.as_raw_fd(), (&raw const addr).cast(), ADDR_LEN) };
        if rc != 0 {
            return Err(io::Error::last_os_error());
        }
        let events = Self { fd };
        events.listen()?;
        events.wait_ack()?;
        Ok(events)
    }

    fn listen(&self) -> io::Result<()> {
        let mut msg = Vec::with_capacity(LISTEN_LEN as usize);
        msg.extend_from_slice(&LISTEN_LEN.to_ne_bytes());
        msg.extend_from_slice(&NLMSG_DONE.to_ne_bytes());
        msg.extend_from_slice(&0_u16.to_ne_bytes());
        msg.extend_from_slice(&1_u32.to_ne_bytes());
        msg.extend_from_slice(&std::process::id().to_ne_bytes());
        msg.extend_from_slice(&CN_IDX_PROC.to_ne_bytes());
        msg.extend_from_slice(&CN_VAL_PROC.to_ne_bytes());
        msg.extend_from_slice(&1_u32.to_ne_bytes());
        msg.extend_from_slice(&0_u32.to_ne_bytes());
        msg.extend_from_slice(&4_u16.to_ne_bytes());
        msg.extend_from_slice(&0_u16.to_ne_bytes());
        msg.extend_from_slice(&PROC_CN_MCAST_LISTEN.to_ne_bytes());
        // SAFETY: `msg` is live for the call and its length is passed with it.
        let sent = unsafe { libc::send(self.fd.as_raw_fd(), msg.as_ptr().cast(), msg.len(), 0) };
        if sent < 0 {
            return Err(io::Error::last_os_error());
        }
        Ok(())
    }

    /// Refused listening shows only in the ack, not in send.
    fn wait_ack(&self) -> io::Result<()> {
        let mut buf = [0_u8; BUFFER];
        loop {
            let mut poll = libc::pollfd {
                fd: self.fd.as_raw_fd(),
                events: libc::POLLIN,
                revents: 0,
            };
            // SAFETY: one live pollfd, and the count says so.
            let ready = unsafe { libc::poll(&raw mut poll, 1, ACK_WAIT_MS) };
            if ready < 0 {
                let e = io::Error::last_os_error();
                if e.kind() == io::ErrorKind::Interrupted {
                    continue;
                }
                return Err(e);
            }
            if ready == 0 {
                return Err(io::Error::from(io::ErrorKind::TimedOut));
            }
            let Some(n) = self.receive(&mut buf)? else {
                continue;
            };
            match ack_error(&buf[..n]) {
                None => {}
                Some(0) => return Ok(()),
                Some(err) => return Err(io::Error::from_raw_os_error(err.cast_signed())),
            }
        }
    }

    /// One message from the kernel into `buf`: its length, Some(0) for one from anyone else, None
    /// when nothing is waiting. ENOBUFS comes back as the error it is.
    fn receive(&self, buf: &mut [u8]) -> io::Result<Option<usize>> {
        // SAFETY: sockaddr_nl is plain data, so all zeroes is a value.
        let mut from: libc::sockaddr_nl = unsafe { zeroed() };
        let mut len = ADDR_LEN;
        // SAFETY: `buf` and `from` are live, and their lengths are passed with them.
        let n = unsafe {
            libc::recvfrom(
                self.fd.as_raw_fd(),
                buf.as_mut_ptr().cast(),
                buf.len(),
                libc::MSG_DONTWAIT,
                (&raw mut from).cast(),
                &raw mut len,
            )
        };
        let Ok(n) = usize::try_from(n) else {
            let e = io::Error::last_os_error();
            return match e.kind() {
                io::ErrorKind::WouldBlock | io::ErrorKind::Interrupted => Ok(None),
                _ => Err(e),
            };
        };
        Ok(Some(if from.nl_pid == 0 { n } else { 0 }))
    }

    /// Hand every waiting event to `matters` and report whether a pass has to walk /proc: one did
    /// matter, or the socket overflowed and events are gone. A hard error is returned; the daemon
    /// then drops the socket.
    pub fn drain(&self, mut matters: impl FnMut(&Event) -> bool) -> io::Result<bool> {
        let mut buf = [0_u8; BUFFER];
        let mut walk = false;
        loop {
            match self.receive(&mut buf) {
                Ok(None) => return Ok(walk),
                Ok(Some(n)) => {
                    if !walk && let Some(event) = parse(&buf[..n]) {
                        walk = matters(&event);
                    }
                }
                Err(e) if e.raw_os_error() == Some(libc::ENOBUFS) => walk = true,
                Err(e) => return Err(e),
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn message(what: u32, data: &[u8]) -> Vec<u8> {
        let mut m = vec![0_u8; NLMSG_HDR];
        m.extend_from_slice(&CN_IDX_PROC.to_ne_bytes());
        m.extend_from_slice(&CN_VAL_PROC.to_ne_bytes());
        m.extend_from_slice(&[0_u8; 12]);
        m.extend_from_slice(&what.to_ne_bytes());
        m.extend_from_slice(&[0_u8; 12]);
        m.extend_from_slice(data);
        m
    }

    fn words(values: &[i32]) -> Vec<u8> {
        values.iter().flat_map(|v| v.to_ne_bytes()).collect()
    }

    #[test]
    fn reads_the_events_it_uses() {
        let fork = message(PROC_EVENT_FORK, &words(&[1, 2, 30, 40]));
        assert_eq!(parse(&fork), Some(Event::Fork { pid: 40, tid: 30 }));
        let exec = message(PROC_EVENT_EXEC, &words(&[5, 6]));
        assert_eq!(parse(&exec), Some(Event::Exec { pid: 6 }));
        let exit = message(PROC_EVENT_EXIT, &words(&[7, 8, 0, 0]));
        assert_eq!(parse(&exit), Some(Event::Exit { tid: 7 }));
    }

    #[test]
    fn a_comm_ends_at_its_nul_or_at_sixteen_bytes() {
        let mut short = words(&[1, 2]);
        short.extend_from_slice(b"cam\0\0\0\0\0\0\0\0\0\0\0\0\0");
        assert_eq!(
            parse(&message(PROC_EVENT_COMM, &short)),
            Some(Event::Comm {
                comm: b"cam".to_vec()
            })
        );
        let mut full = words(&[1, 2]);
        full.extend_from_slice(b"0123456789abcdef");
        assert_eq!(
            parse(&message(PROC_EVENT_COMM, &full)),
            Some(Event::Comm {
                comm: b"0123456789abcdef".to_vec()
            })
        );
    }

    #[test]
    fn a_short_or_foreign_message_is_no_event() {
        let whole = message(PROC_EVENT_FORK, &words(&[1, 2, 30, 40]));
        for n in 0..whole.len() {
            assert_eq!(parse(&whole[..n]), None, "{n} bytes");
        }
        let mut foreign = whole.clone();
        foreign[NLMSG_HDR] = 9;
        assert_eq!(parse(&foreign), None);
        assert_eq!(parse(&message(0x80, &words(&[1, 2, 3, 4]))), None);
    }

    #[test]
    fn an_ack_carries_its_error() {
        assert_eq!(
            ack_error(&message(PROC_EVENT_NONE, &words(&[1, 0]))),
            Some(1)
        );
        assert_eq!(ack_error(&message(PROC_EVENT_FORK, &words(&[0, 0]))), None);
    }
}
