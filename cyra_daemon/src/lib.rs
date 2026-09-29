//! cyrad: sets the threads a chain file matches by name, moves reset children off their stage's
//! cpus, reports drift, and records what it holds so `cyra status` sees it.

#[allow(unsafe_code)]
pub mod ffi;
pub mod reconcile;
pub mod signals;
pub mod state;
