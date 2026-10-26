// Compiles the C++ half of the bridge against capa_core's headers, then links what CMake resolved.
// CAPA_CORE_LINK is a ':'-separated list of library files: capa_core's archive and what that
// archive needs. `cargo check` and `cargo clippy` link nothing, so they run without it.

use std::env;
use std::path::Path;

fn main() {
    let root = Path::new(env!("CARGO_MANIFEST_DIR"));
    let include = root.join("../capa_core/include");
    cxx_build::bridge("src/ffi.rs")
        .file("src/bridge.cpp")
        .include(&include)
        .include(root.join("src"))
        .std("c++17")
        .flag("-Wall")
        .flag("-Wextra")
        .compile("capad_bridge");

    let link = env::var("CAPA_CORE_LINK").unwrap_or_default();
    for lib in link.split(':').filter(|s| !s.is_empty()) {
        let path = Path::new(lib);
        let dir = path.parent().expect("a library path has a directory");
        let file = path
            .file_name()
            .and_then(|f| f.to_str())
            .expect("a library file name");
        let stem = file.strip_prefix("lib").unwrap_or(file);
        let (kind, name) = match stem.split_once(".so") {
            Some((name, _)) => ("dylib", name),
            None => ("static", stem.trim_end_matches(".a")),
        };
        println!("cargo:rustc-link-search=native={}", dir.display());
        println!("cargo:rustc-link-lib={kind}={name}");
    }
    // capa_core's archive comes after the bridge on the link line and needs libstdc++ too.
    println!("cargo:rustc-link-lib=stdc++");

    println!("cargo:rerun-if-env-changed=CAPA_CORE_LINK");
    println!("cargo:rerun-if-changed=src/bridge.cpp");
    println!("cargo:rerun-if-changed=src/bridge.hpp");
    println!("cargo:rerun-if-changed={}", include.display());
}
