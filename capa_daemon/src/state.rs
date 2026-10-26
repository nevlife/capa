//! The daemon's state file, `capad.state` in the daemon's directory: what this run set or moved,
//! with what each thread ran at before. A capad restarted after a crash reads it, so it can still
//! put those threads back where they started rather than where the crashed run left them.

use std::fs::{self, OpenOptions};
use std::io::{ErrorKind, Read, Write};
use std::os::unix::fs::{MetadataExt, OpenOptionsExt};
use std::path::PathBuf;

use crate::ffi;

const NAME: &str = "capad.state";

fn dir() -> Result<PathBuf, String> {
    ffi::daemon_dir()
        .map(PathBuf::from)
        .map_err(|e| e.what().to_owned())
}

/// Replace the file whole: written aside, then renamed over it.
pub fn write(text: &str) -> Result<(), String> {
    let dir = dir()?;
    let tmp = dir.join(format!(".{NAME}.{}", std::process::id()));
    let _ = fs::remove_file(&tmp);
    let mut f = OpenOptions::new()
        .write(true)
        .create_new(true)
        .mode(0o644)
        .custom_flags(libc::O_NOFOLLOW)
        .open(&tmp)
        .map_err(|e| format!("{}: {e}", tmp.display()))?;
    f.write_all(text.as_bytes())
        .map_err(|e| format!("{}: {e}", tmp.display()))?;
    fs::rename(&tmp, dir.join(NAME)).map_err(|e| format!("{}: {e}", dir.display()))
}

/// The file's text, or None when there is none. Believed only as a regular file of the
/// directory's owner that no one else can write, like the daemon's record.
pub fn read() -> Result<Option<String>, String> {
    let dir = dir()?;
    let path = dir.join(NAME);
    let mut f = match OpenOptions::new()
        .read(true)
        .custom_flags(libc::O_NOFOLLOW | libc::O_NONBLOCK)
        .open(&path)
    {
        Ok(f) => f,
        Err(e) if e.kind() == ErrorKind::NotFound => return Ok(None),
        Err(e) => return Err(format!("{}: {e}", path.display())),
    };
    let meta = f.metadata().map_err(|e| e.to_string())?;
    let owner = fs::metadata(&dir).map_err(|e| e.to_string())?.uid();
    if !meta.is_file() || meta.uid() != owner || meta.mode() & 0o022 != 0 {
        return Err(format!(
            "{} is not a file of {}'s owner that only it can write",
            path.display(),
            dir.display()
        ));
    }
    let mut text = String::new();
    f.read_to_string(&mut text)
        .map_err(|e| format!("{}: {e}", path.display()))?;
    Ok(Some(text))
}
