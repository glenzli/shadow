use std::{env, path::PathBuf};

fn main() {
    let crate_root = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").expect("crate root"));
    let repository_root = crate_root.join("../..");
    let image_root = repository_root.join("cpp/shadow-image");
    let image_include = image_root.join("include");
    let libraw = pkg_config::Config::new()
        .atleast_version("0.22.0")
        .cargo_metadata(false)
        .probe("libraw")
        .expect("LibRaw 0.22+ must be discoverable through pkg-config");
    let libjpeg = pkg_config::Config::new()
        .cargo_metadata(false)
        .probe("libjpeg")
        .expect("libjpeg-turbo must be discoverable through pkg-config");

    let mut build = cxx_build::bridge("src/lib.rs");
    build
        .file(image_root.join("src/bridge/cxx_bridge.cpp"))
        .file(image_root.join("src/decoder/libraw_decoder.cpp"))
        .file(image_root.join("src/proxy/jpeg_proxy.cpp"))
        .include(&image_include)
        .std("c++20");

    let target_family = env::var("CARGO_CFG_TARGET_FAMILY").unwrap_or_default();
    for include_path in &libraw.include_paths {
        if target_family == "unix" {
            build
                .flag("-isystem")
                .flag(include_path.to_string_lossy().as_ref());
        } else {
            build.include(include_path);
        }
    }
    for include_path in &libjpeg.include_paths {
        if target_family == "unix" {
            build
                .flag("-isystem")
                .flag(include_path.to_string_lossy().as_ref());
        } else {
            build.include(include_path);
        }
    }

    if env::var("CARGO_CFG_TARGET_ENV").as_deref() == Ok("msvc") {
        build.flag("/W4").flag("/permissive-");
    } else {
        build
            .flag("-Wall")
            .flag("-Wextra")
            .flag("-Wpedantic")
            .flag("-Wconversion")
            .flag("-Wsign-conversion");
    }
    build.compile("shadow-bridge-cxx");

    for link_path in &libraw.link_paths {
        println!("cargo:rustc-link-search=native={}", link_path.display());
    }
    for library in &libraw.libs {
        if target_family == "unix" && library == "stdc++" {
            continue;
        }
        println!("cargo:rustc-link-lib={library}");
    }
    for framework_path in &libraw.framework_paths {
        println!(
            "cargo:rustc-link-search=framework={}",
            framework_path.display()
        );
    }
    for framework in &libraw.frameworks {
        println!("cargo:rustc-link-lib=framework={framework}");
    }
    for link_path in &libjpeg.link_paths {
        println!("cargo:rustc-link-search=native={}", link_path.display());
    }
    for library in &libjpeg.libs {
        println!("cargo:rustc-link-lib={library}");
    }

    println!("cargo:rerun-if-changed=src/lib.rs");
    println!(
        "cargo:rerun-if-changed={}",
        image_root
            .join("include/shadow/image/decoder.hpp")
            .display()
    );
    println!(
        "cargo:rerun-if-changed={}",
        image_root
            .join("include/shadow/image/cxx_bridge.hpp")
            .display()
    );
    println!(
        "cargo:rerun-if-changed={}",
        image_root.join("src/bridge/cxx_bridge.cpp").display()
    );
    println!(
        "cargo:rerun-if-changed={}",
        image_root.join("src/decoder/libraw_decoder.cpp").display()
    );
    println!(
        "cargo:rerun-if-changed={}",
        image_root.join("src/proxy/jpeg_proxy.cpp").display()
    );
}
