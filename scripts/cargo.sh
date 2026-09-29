#!/usr/bin/env bash
# cargo on cyra_daemon with rustup's cargo, so cyra_daemon/rust-toolchain.toml picks the toolchain.
# A distribution cargo on PATH ignores that file and is too old for the edition.
set -eu
home="${CARGO_HOME:-$HOME/.cargo}"
cargo="${CARGO:-}"
if [ -z "$cargo" ]; then
  if [ -x "$home/bin/cargo" ]; then cargo="$home/bin/cargo"; else cargo=cargo; fi
fi
cd "$(dirname "$0")/../cyra_daemon"
exec "$cargo" "$@"
