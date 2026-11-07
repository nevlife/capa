#!/usr/bin/env bash
# capad's tests under a sanitizer: nightly -Zsanitizer with std rebuilt the same way. Only the Rust
# is instrumented; capa_core is linked as built, from CAPA_CORE_LINK (as capa_daemon/build.rs reads
# it: capa_core's archive and what it needs, ':'-separated).
#   scripts/sanitize.sh address|thread [extra cargo test arguments]
set -eu
cd "$(dirname "$0")/../capa_daemon"
kind=${1:?address or thread}
shift
: "${CAPA_CORE_LINK:?set to the capa_core archive and its libraries}"
toolchain=nightly-2026-10-05
host=$(rustc "+$toolchain" -vV | sed -n 's/^host: //p')
target=${SANITIZE_TARGET:-target}/$kind
# A record is trusted only in a directory nobody else can write to.
run=$(mktemp -d)
trap 'rm -rf "$run"' EXIT
install -d -m 755 "$run/bind" "$run/daemon"
export CAPA_RUN_DIR=$run/bind CAPA_DAEMON_DIR=$run/daemon
# The test harness unwinds; a profile that aborts would build std both ways.
export CARGO_PROFILE_DEV_PANIC=unwind
export RUSTFLAGS="-Zsanitizer=$kind"
export RUSTDOCFLAGS="$RUSTFLAGS"
export TSAN_OPTIONS="halt_on_error=1:second_deadlock_stack=1"
cargo "+$toolchain" test -Zbuild-std --target "$host" --target-dir "$target" --locked --no-fail-fast "$@"
