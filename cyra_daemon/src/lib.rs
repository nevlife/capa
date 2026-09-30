//! cyrad: sets the threads a chain file matches by name, moves reset children off their stage's
//! cpus, reports drift, and records what it holds so `cyra status` sees it.

// The bridge's unsafe blocks are the ones cxx generates.
#[allow(unsafe_code, clippy::multiple_unsafe_ops_per_block)]
pub mod ffi;
pub mod reconcile;
pub mod signals;
pub mod state;
