//! capad [--spec FILE] [--interval SEC] [--once]
//!
//! Between passes it reads the kernel's process events, and a pass walks /proc only when one of
//! them can change what a matched stage finds, or once every RESCAN. Without the events every pass
//! walks /proc.
//!
//! SIGHUP rereads the file (a file that breaks a rule is refused and the old one kept). SIGTERM and
//! SIGINT put every thread the daemon set back and exit. --once runs one pass and exits, leaving
//! what it set in place. Either way it starts by taking over what a crashed run left.

use std::process::ExitCode;
use std::time::{Duration, Instant};

use capad::procevents::ProcEvents;
use capad::reconcile::Reconciler;
use capad::signals::{Signal, Signals};

/// How long a pass may go without walking /proc, for what the events cannot say: a matched thread
/// that came and went while the socket was being read, or a kernel that stopped sending.
const RESCAN: Duration = Duration::from_secs(30);

fn print(lines: Vec<String>) {
    for l in lines {
        eprintln!("capad: {l}");
    }
}

fn usage() -> ExitCode {
    eprintln!("usage: capad [--spec FILE] [--interval SEC] [--once]");
    ExitCode::from(2)
}

fn main() -> ExitCode {
    let mut path = None;
    let mut interval = 1.0_f64;
    let mut once = false;
    let mut args = std::env::args().skip(1);
    while let Some(a) = args.next() {
        match a.as_str() {
            "--spec" => match args.next() {
                Some(p) => path = Some(p),
                None => return usage(),
            },
            "--interval" => match args.next().and_then(|s| s.parse::<f64>().ok()) {
                Some(s) if s > 0.0 && s.is_finite() => interval = s,
                _ => return usage(),
            },
            "--once" => once = true,
            _ => return usage(),
        }
    }
    let path = path.or_else(|| std::env::var("CAPA_SPEC").ok());
    let Some(path) = path.filter(|p| !p.is_empty()) else {
        eprintln!("capad: no chain file: pass --spec, or set CAPA_SPEC");
        return ExitCode::from(2);
    };

    let mut rec = match Reconciler::load(&path) {
        Ok(r) => r,
        Err(e) => {
            eprintln!("capad: {e}");
            return ExitCode::from(1);
        }
    };
    if once {
        print(rec.recover());
        print(rec.pass());
        return ExitCode::SUCCESS;
    }

    // Blocked before recover, so a reload sent at start waits instead of killing the daemon.
    let signals = match Signals::block() {
        Ok(s) => s,
        Err(e) => {
            eprintln!("capad: pthread_sigmask: {e}");
            return ExitCode::from(1);
        }
    };
    print(rec.recover());
    eprintln!("capad: watching {path} every {interval}s");
    let mut events = match ProcEvents::open() {
        Ok(e) => Some(e),
        Err(e) => {
            eprintln!("capad: no process events ({e}), walking /proc every pass");
            None
        }
    };
    let mut walk = true;
    let mut walked = Instant::now();
    loop {
        if let Some(socket) = &events {
            match socket.drain(|event| rec.matters(event)) {
                Ok(needed) => walk |= needed,
                Err(e) => {
                    eprintln!("capad: process events lost ({e}), walking /proc every pass");
                    events = None;
                }
            }
        }
        if walk || events.is_none() || walked.elapsed() >= RESCAN {
            print(rec.pass());
            walked = Instant::now();
            walk = false;
        } else {
            print(rec.pass_quiet());
        }
        match signals.wait(Duration::from_secs_f64(interval)) {
            Ok(None) => {}
            Ok(Some(Signal::Reload)) => match Reconciler::load(&path) {
                Ok(mut next) => {
                    print(next.adopt(&mut rec));
                    rec = next;
                    walk = true;
                    eprintln!("capad: reloaded {path}");
                }
                Err(e) => eprintln!("capad: reload refused, keeping the old file: {e}"),
            },
            Ok(Some(Signal::Stop)) => {
                print(rec.restore());
                return ExitCode::SUCCESS;
            }
            Err(e) => {
                eprintln!("capad: sigtimedwait: {e}");
                return ExitCode::from(1);
            }
        }
    }
}
