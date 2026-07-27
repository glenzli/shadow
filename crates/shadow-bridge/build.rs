use std::{env, path::PathBuf};

const BRIDGE_INPUTS: &[&str] = &[
    "include/shadow/image/decoder.hpp",
    "include/shadow/image/private_decoder_plugin.hpp",
    "include/shadow/image/display_luma.hpp",
    "include/shadow/image/display_output.hpp",
    "include/shadow/image/sensor_clipping.hpp",
    "include/shadow/image/adjustment_execution.hpp",
    "include/shadow/image/adjustment_graph.hpp",
    "include/shadow/image/adjustment_layers.hpp",
    "include/shadow/image/adjustment_parameters.hpp",
    "include/shadow/image/edit.hpp",
    "include/shadow/image/edit_execution_plan.hpp",
    "include/shadow/image/edited_proxy_rendering.hpp",
    "include/shadow/image/full_edit_detail.hpp",
    "include/shadow/image/lut.hpp",
    "include/shadow/image/optics.hpp",
    "include/shadow/image/photo_geometry.hpp",
    "include/shadow/image/proxy_rendering.hpp",
    "include/shadow/image/raw_development.hpp",
    "include/shadow/image/raw_development_plan.hpp",
    "include/shadow/image/raw_development_receipt.hpp",
    "include/shadow/image/raw_pipeline.hpp",
    "include/shadow/image/camera_profile.hpp",
    "include/shadow/image/camera_profile_catalog.hpp",
    "include/shadow/image/dcp_color_development.hpp",
    "include/shadow/image/fused_raw_development.hpp",
    "include/shadow/image/raw_denoise.hpp",
    "include/shadow/image/cxx_bridge.hpp",
    "include/shadow/image/color_management.hpp",
    "include/shadow/image/source_rendering.hpp",
    "include/shadow/image/source_profile_catalog.hpp",
    "include/shadow/image/warm_edit_preview.hpp",
    "include/shadow/image/working_rgb.hpp",
    "src/bridge/cxx_bridge.cpp",
];

const EMBEDDED_IMAGE_INPUTS: &[&str] = &[
    "src/decoder/libraw_decoder.cpp",
    "src/decoder/raster_exif.hpp",
    "src/decoder/raster_exif.cpp",
    "src/decoder/raster_decoder.cpp",
    "src/decoder/heif_decoder.hpp",
    "src/decoder/heif_decoder.cpp",
    "src/decoder/decode_session_isolation.hpp",
    "src/decoder/decode_session_isolation.cpp",
    "src/decoder/photo_decoder_router.cpp",
    "src/decoder/private_decoder_plugin.cpp",
    "src/color/lcms_color_management.cpp",
    "src/color/source_profile_catalog.cpp",
    "src/color/source_rendering.cpp",
    "src/edit/cube_lut.cpp",
    "src/edit/adjustment_execution.cpp",
    "src/edit/metal_adjustment_execution.hpp",
    "src/edit/metal_adjustment_program.hpp",
    "src/edit/cpu_reference.cpp",
    "src/edit/cpu_reference_color.ipp",
    "src/edit/cpu_reference_curve.ipp",
    "src/edit/cpu_reference_detail.ipp",
    "src/edit/cpu_reference_tone.ipp",
    "src/edit/local_mask.cpp",
    "src/edit/photo_geometry.cpp",
    "src/edit/retouch.cpp",
    "src/edit/metal_adjustment.mm",
    "src/edit/metal_adjustment_msl.hpp",
    "src/edit/metal_adjustment_stub.cpp",
    "src/concurrency/row_scheduler.hpp",
    "src/concurrency/row_scheduler.cpp",
    "src/raw/bayer_demosaic.cpp",
    "src/raw/bayer_sampling.hpp",
    "src/raw/bayer_sampling.cpp",
    "src/raw/camera_profile_catalog.cpp",
    "src/raw/dcp_color_development.cpp",
    "src/raw/dcp_parser.cpp",
    "src/raw/fused_raw_development.cpp",
    "src/raw/metal_raw_development.hpp",
    "src/raw/metal_raw_development.mm",
    "src/raw/metal_raw_development_stub.cpp",
    "src/raw/raw_denoise.cpp",
    "src/raw/raw_pipeline.cpp",
    "src/raw/sensor_clipping.cpp",
    "src/optics/lensfun_optics.cpp",
    "src/proxy/developed_source_raster.hpp",
    "src/proxy/developed_source_raster.cpp",
    "src/proxy/display_output.cpp",
    "src/proxy/display_rgb_math.hpp",
    "src/proxy/edited_proxy_rendering.cpp",
    "src/proxy/full_edit_detail.cpp",
    "src/proxy/jpeg_display_luma.cpp",
    "src/proxy/jpeg_proxy_encoding.hpp",
    "src/proxy/jpeg_proxy_encoding.cpp",
    "src/proxy/proxy_rendering.cpp",
    "src/proxy/proxy_render_request_validation.hpp",
    "src/proxy/proxy_render_request_validation.cpp",
    "src/proxy/warm_edit_preview.cpp",
    "src/proxy/metal_display_output.hpp",
    "src/proxy/metal_display_output.mm",
    "src/proxy/metal_display_output_stub.cpp",
    "src/proxy/warm_edit_gpu.hpp",
    "src/proxy/warm_edit_gpu_dispatcher.hpp",
    "src/proxy/warm_edit_gpu_dispatcher.mm",
    "src/proxy/warm_edit_gpu_kernel_contract.hpp",
    "src/proxy/warm_edit_gpu_msl.hpp",
    "src/proxy/warm_edit_gpu_pipeline_context.hpp",
    "src/proxy/warm_edit_gpu_pipeline_context.mm",
    "src/proxy/warm_edit_gpu_resident_resources.hpp",
    "src/proxy/warm_edit_gpu_resident_resources.mm",
    "src/proxy/warm_edit_gpu_render_plan.hpp",
    "src/proxy/warm_edit_gpu_render_plan.cpp",
    "src/proxy/warm_edit_gpu.mm",
    "src/proxy/warm_edit_gpu_stub.cpp",
];

fn parse_flag(name: &str, default: bool) -> bool {
    match env::var(name).ok().as_deref() {
        None => default,
        Some("1" | "ON" | "on" | "true" | "TRUE") => true,
        Some("0" | "OFF" | "off" | "false" | "FALSE") => false,
        Some(value) => {
            panic!("{name} must be 0/1, OFF/ON, or false/true; received {value}")
        }
    }
}

macro_rules! configure_warnings {
    ($build:expr) => {
        if env::var("CARGO_CFG_TARGET_ENV").as_deref() == Ok("msvc") {
            $build.flag("/W4").flag("/permissive-");
        } else {
            $build
                .flag("-Wall")
                .flag("-Wextra")
                .flag("-Wpedantic")
                .flag("-Wconversion")
                .flag("-Wsign-conversion");
        }
    };
}

fn track_inputs(image_root: &std::path::Path, inputs: &[&str]) {
    for relative_path in inputs {
        println!(
            "cargo:rerun-if-changed={}",
            image_root.join(relative_path).display()
        );
    }
}

#[allow(clippy::too_many_lines)] // Native source tracking stays beside the matching CXX build.
fn main() {
    // Direct Cargo builds may select optional native dependencies through pkg-config. Desktop
    // CMake builds still use pkg-config here for the cache-visible libjpeg identity, while CMake
    // itself owns all implementation dependencies.
    println!("cargo:rerun-if-env-changed=PKG_CONFIG_PATH");
    println!("cargo:rerun-if-env-changed=SHADOW_ENABLE_METAL");
    println!("cargo:rerun-if-env-changed=SHADOW_BRIDGE_EXTERNAL_IMAGE");
    let crate_root = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").expect("crate root"));
    let repository_root = crate_root.join("../..");
    let image_root = repository_root.join("cpp/shadow-image");
    let image_include = image_root.join("include");
    let external_image = parse_flag("SHADOW_BRIDGE_EXTERNAL_IMAGE", false);

    // The Rust preprocessing identity must describe the libjpeg used by the final process in
    // both build modes. In CMake desktop builds this is metadata-only; the bridge does not compile
    // or link the image implementation itself.
    let libjpeg = pkg_config::Config::new()
        .cargo_metadata(false)
        .probe("libjpeg")
        .expect("libjpeg-turbo must be discoverable through pkg-config");
    println!(
        "cargo:rustc-env=SHADOW_LIBJPEG_TURBO_VERSION={}",
        libjpeg.version
    );

    let mut build = cxx_build::bridge("src/lib.rs");
    build
        .file(image_root.join("src/bridge/cxx_bridge.cpp"))
        .include(&image_include)
        .std("c++20");

    if external_image {
        // CMake already owns Shadow::Image for desktop builds. Keep only the generated CXX glue
        // and the coarse bridge shim in Cargo's archive; the final Qt target resolves their native
        // calls through the single CMake-built image library.
        configure_warnings!(&mut build);
        build.compile("shadow-bridge-cxx");
        println!("cargo:rerun-if-changed=src/lib.rs");
        track_inputs(&image_root, BRIDGE_INPUTS);
        return;
    }

    // Direct Cargo builds remain self-contained. Independent decoder sessions require the
    // reentrant LibRaw build; the non-reentrant Unix/macOS library is never an acceptable fallback.
    let libraw = pkg_config::Config::new()
        .atleast_version("0.22.0")
        .cargo_metadata(false)
        .probe("libraw_r")
        .expect("thread-safe LibRaw 0.22+ (pkg-config libraw_r) must be discoverable");
    if !libraw.libs.iter().any(|library| library == "raw_r")
        || libraw.libs.iter().any(|library| library == "raw")
    {
        panic!(
            "pkg-config libraw_r must resolve to the reentrant raw_r library, resolved libraries: {:?}",
            libraw.libs
        );
    }
    let lensfun = pkg_config::Config::new()
        .cargo_metadata(false)
        .probe("lensfun")
        .ok();
    let lcms2 = pkg_config::Config::new()
        .cargo_metadata(false)
        .probe("lcms2")
        .expect("LittleCMS 2 must be discoverable through pkg-config");
    let libheif = pkg_config::Config::new()
        .atleast_version("1.19.7")
        .cargo_metadata(false)
        .probe("libheif")
        .ok();
    let target_family = env::var("CARGO_CFG_TARGET_FAMILY").unwrap_or_default();
    let target_os = env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    let metal_enabled = parse_flag("SHADOW_ENABLE_METAL", target_os == "macos");
    assert!(
        !metal_enabled || target_os == "macos",
        "SHADOW_ENABLE_METAL=1 is supported only for a macOS target"
    );

    build
        .file(image_root.join("src/decoder/libraw_decoder.cpp"))
        .file(image_root.join("src/decoder/raster_exif.cpp"))
        .file(image_root.join("src/decoder/raster_decoder.cpp"))
        .file(image_root.join("src/decoder/heif_decoder.cpp"))
        .file(image_root.join("src/decoder/decode_session_isolation.cpp"))
        .file(image_root.join("src/decoder/photo_decoder_router.cpp"))
        .file(image_root.join("src/decoder/private_decoder_plugin.cpp"))
        .file(image_root.join("src/color/lcms_color_management.cpp"))
        .file(image_root.join("src/color/source_profile_catalog.cpp"))
        .file(image_root.join("src/color/source_rendering.cpp"))
        .file(image_root.join("src/edit/adjustment_execution.cpp"))
        .file(image_root.join("src/edit/cube_lut.cpp"))
        .file(image_root.join("src/edit/cpu_reference.cpp"))
        .file(image_root.join("src/edit/local_mask.cpp"))
        .file(image_root.join("src/edit/photo_geometry.cpp"))
        .file(image_root.join("src/edit/retouch.cpp"))
        .file(image_root.join("src/concurrency/row_scheduler.cpp"))
        .file(image_root.join("src/raw/bayer_demosaic.cpp"))
        .file(image_root.join("src/raw/bayer_sampling.cpp"))
        .file(image_root.join("src/raw/camera_profile_catalog.cpp"))
        .file(image_root.join("src/raw/dcp_color_development.cpp"))
        .file(image_root.join("src/raw/dcp_parser.cpp"))
        .file(image_root.join("src/raw/fused_raw_development.cpp"))
        .file(image_root.join("src/raw/raw_denoise.cpp"))
        .file(image_root.join("src/raw/raw_pipeline.cpp"))
        .file(image_root.join("src/raw/sensor_clipping.cpp"))
        .file(image_root.join("src/optics/lensfun_optics.cpp"))
        .file(image_root.join("src/proxy/developed_source_raster.cpp"))
        .file(image_root.join("src/proxy/display_output.cpp"))
        .file(image_root.join("src/proxy/edited_proxy_rendering.cpp"))
        .file(image_root.join("src/proxy/full_edit_detail.cpp"))
        .file(image_root.join("src/proxy/jpeg_display_luma.cpp"))
        .file(image_root.join("src/proxy/jpeg_proxy_encoding.cpp"))
        .file(image_root.join("src/proxy/proxy_rendering.cpp"))
        .file(image_root.join("src/proxy/proxy_render_request_validation.cpp"))
        .file(image_root.join("src/proxy/warm_edit_gpu_render_plan.cpp"))
        .file(image_root.join("src/proxy/warm_edit_preview.cpp"));
    if metal_enabled {
        build
            .file(image_root.join("src/edit/metal_adjustment.mm"))
            .file(image_root.join("src/raw/metal_raw_development.mm"))
            .file(image_root.join("src/proxy/metal_display_output.mm"))
            .file(image_root.join("src/proxy/warm_edit_gpu_dispatcher.mm"))
            .file(image_root.join("src/proxy/warm_edit_gpu_pipeline_context.mm"))
            .file(image_root.join("src/proxy/warm_edit_gpu_resident_resources.mm"))
            .file(image_root.join("src/proxy/warm_edit_gpu.mm"))
            .define("SHADOW_IMAGE_HAS_METAL", Some("1"));
    } else {
        build
            .file(image_root.join("src/edit/metal_adjustment_stub.cpp"))
            .file(image_root.join("src/raw/metal_raw_development_stub.cpp"))
            .file(image_root.join("src/proxy/metal_display_output_stub.cpp"))
            .file(image_root.join("src/proxy/warm_edit_gpu_stub.cpp"))
            .define("SHADOW_IMAGE_HAS_METAL", Some("0"));
    }

    // Put the selected Lensfun headers before generic Homebrew include roots contributed by
    // LibRaw/LCMS. This keeps the headers and dylib from the same pkg-config identity when a
    // stable and a development Lensfun installation coexist.
    if let Some(lensfun) = &lensfun {
        build.define("SHADOW_IMAGE_HAS_LENSFUN", Some("1"));
        for include_path in &lensfun.include_paths {
            if target_family == "unix" {
                build
                    .flag("-isystem")
                    .flag(include_path.to_string_lossy().as_ref());
            } else {
                build.include(include_path);
            }
        }
    } else {
        build.define("SHADOW_IMAGE_HAS_LENSFUN", Some("0"));
    }

    if let Some(libheif) = &libheif {
        build.define("SHADOW_IMAGE_HAS_LIBHEIF", Some("1"));
        for include_path in &libheif.include_paths {
            if target_family == "unix" {
                build
                    .flag("-isystem")
                    .flag(include_path.to_string_lossy().as_ref());
            } else {
                build.include(include_path);
            }
        }
    } else {
        build.define("SHADOW_IMAGE_HAS_LIBHEIF", Some("0"));
    }

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
    for include_path in &lcms2.include_paths {
        if target_family == "unix" {
            build
                .flag("-isystem")
                .flag(include_path.to_string_lossy().as_ref());
        } else {
            build.include(include_path);
        }
    }
    configure_warnings!(&mut build);
    build.compile("shadow-bridge-cxx");

    if metal_enabled {
        println!("cargo:rustc-link-lib=framework=Metal");
        println!("cargo:rustc-link-lib=framework=Foundation");
    }

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
    for link_path in &lcms2.link_paths {
        println!("cargo:rustc-link-search=native={}", link_path.display());
    }
    for library in &lcms2.libs {
        println!("cargo:rustc-link-lib={library}");
    }
    if let Some(libheif) = &libheif {
        for link_path in &libheif.link_paths {
            println!("cargo:rustc-link-search=native={}", link_path.display());
        }
        for library in &libheif.libs {
            if target_family == "unix" && library == "stdc++" {
                continue;
            }
            println!("cargo:rustc-link-lib={library}");
        }
        for framework_path in &libheif.framework_paths {
            println!(
                "cargo:rustc-link-search=framework={}",
                framework_path.display()
            );
        }
        for framework in &libheif.frameworks {
            println!("cargo:rustc-link-lib=framework={framework}");
        }
    }
    if target_family == "unix" && env::var("CARGO_CFG_TARGET_OS").as_deref() != Ok("macos") {
        println!("cargo:rustc-link-lib=dl");
    }
    if let Some(lensfun) = &lensfun {
        for link_path in &lensfun.link_paths {
            println!("cargo:rustc-link-search=native={}", link_path.display());
        }
        for library in &lensfun.libs {
            if target_family == "unix" && library == "stdc++" {
                continue;
            }
            println!("cargo:rustc-link-lib={library}");
        }
        for framework_path in &lensfun.framework_paths {
            println!(
                "cargo:rustc-link-search=framework={}",
                framework_path.display()
            );
        }
        for framework in &lensfun.frameworks {
            println!("cargo:rustc-link-lib=framework={framework}");
        }
    }

    println!("cargo:rerun-if-changed=src/lib.rs");
    track_inputs(&image_root, BRIDGE_INPUTS);
    track_inputs(&image_root, EMBEDDED_IMAGE_INPUTS);
}
