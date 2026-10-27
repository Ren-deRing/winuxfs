use std::{env, path::PathBuf};

fn main() {
    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let header = manifest.join("../../include/winuxfs/backend.h");
    println!("cargo:rerun-if-changed={}", header.display());

    // Match the target's data model (CARGO_CFG_TARGET_* describes the crate target,
    // not the build host), so layouts are right when cross-compiling.
    let target = env::var("TARGET").unwrap();
    let mut b = bindgen::Builder::default()
        .header(header.to_str().unwrap())
        .clang_arg(format!("--target={target}"))
        .layout_tests(true)
        .allowlist_file(".*backend\\.h")
        .derive_default(false)
        .parse_callbacks(Box::new(bindgen::CargoCallbacks::new()));
    if env::var("CARGO_CFG_TARGET_OS").unwrap() == "windows" {
        // Headers (stdint/stddef) come from clang's builtin resource dir.
        b = b.clang_arg("-fms-extensions");
    }
    let out = PathBuf::from(env::var("OUT_DIR").unwrap()).join("bindings.rs");
    b.generate().expect("bindgen failed").write_to_file(out).unwrap();
}
