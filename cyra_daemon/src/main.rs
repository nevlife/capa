//! cyrad [--spec FILE] [--interval SEC] [--once]
//!
//! SIGHUP rereads the file (a file that breaks a rule is refused and the old one kept). SIGTERM and
//! SIGINT put every thread the daemon set back and exit. --once runs one pass and exits, leaving
//! what it set in place.

use std::process::ExitCode;
use std::time::Duration;

use cyrad::reconcile::Reconciler;
use cyrad::signals::{Signal, Signals};

fn print(lines: Vec<String>) {
    for l in lines {
        eprintln!("cyrad: {l}");
    }
}

fn usage() -> ExitCode {
    eprintln!("usage: cyrad [--spec FILE] [--interval SEC] [--once]");
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
    let path = path.or_else(|| std::env::var("CYRA_SPEC").ok());
    let Some(path) = path.filter(|p| !p.is_empty()) else {
        eprintln!("cyrad: no chain file: pass --spec, or set CYRA_SPEC");
        return ExitCode::from(2);
    };

    let mut rec = match Reconciler::load(&path) {
        Ok(r) => r,
        Err(e) => {
            eprintln!("cyrad: {e}");
            return ExitCode::from(1);
        }
    };
    if once {
        print(rec.pass());
        return ExitCode::SUCCESS;
    }

    let signals = match Signals::block() {
        Ok(s) => s,
        Err(e) => {
            eprintln!("cyrad: pthread_sigmask: {e}");
            return ExitCode::from(1);
        }
    };
    eprintln!("cyrad: watching {path} every {interval}s");
    loop {
        print(rec.pass());
        match signals.wait(Duration::from_secs_f64(interval)) {
            Ok(None) => {}
            Ok(Some(Signal::Reload)) => match Reconciler::load(&path) {
                Ok(mut next) => {
                    print(next.adopt(&mut rec));
                    rec = next;
                    eprintln!("cyrad: reloaded {path}");
                }
                Err(e) => eprintln!("cyrad: reload refused, keeping the old file: {e}"),
            },
            Ok(Some(Signal::Stop)) => {
                print(rec.restore());
                return ExitCode::SUCCESS;
            }
            Err(e) => {
                eprintln!("cyrad: sigtimedwait: {e}");
                return ExitCode::from(1);
            }
        }
    }
}
