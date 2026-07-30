use std::{
    collections::BTreeSet,
    env, fs,
    path::{Path, PathBuf},
};

const BRIDGE_SOURCES: &[&str] = &[
    "src/bridge/adjustment_render_wire.cpp",
    "src/bridge/cxx_bridge.cpp",
    "src/bridge/cxx_handle.cpp",
    "src/bridge/cxx_preview_frame.cpp",
];

const BRIDGE_ADDITIONAL_INPUTS: &[&str] = &[
    "include/shadow/image/decoder.hpp",
    "include/shadow/image/private_decoder_plugin.hpp",
    "include/shadow/image/display_luma.hpp",
    "include/shadow/image/display_output.hpp",
    "include/shadow/image/sensor_clipping.hpp",
    "include/shadow/image/adjustment_execution.hpp",
    "include/shadow/image/adjustment_graph.hpp",
    "include/shadow/image/adjustment_layers.hpp",
    "include/shadow/image/adjustment_parameters.hpp",
    "include/shadow/image/cpu_edit_reference.hpp",
    "include/shadow/image/edit.hpp",
    "include/shadow/image/edit_error.hpp",
    "include/shadow/image/edit_execution_plan.hpp",
    "include/shadow/image/edited_proxy_rendering.hpp",
    "include/shadow/image/full_edit_detail.hpp",
    "include/shadow/image/lut.hpp",
    "include/shadow/image/optics.hpp",
    "include/shadow/image/photo_geometry.hpp",
    "include/shadow/image/photo_liquify.hpp",
    "include/shadow/image/photo_structural_rendering.hpp",
    "include/shadow/image/proxy_rendering.hpp",
    "include/shadow/image/raw_development.hpp",
    "include/shadow/image/raw_development_plan.hpp",
    "include/shadow/image/raw_development_receipt.hpp",
    "include/shadow/image/raw_pipeline.hpp",
    "include/shadow/image/retouch.hpp",
    "include/shadow/image/tone_curve.hpp",
    "include/shadow/image/camera_profile.hpp",
    "include/shadow/image/camera_profile_catalog.hpp",
    "include/shadow/image/dcp_color_development.hpp",
    "include/shadow/image/edit_preview_frame.hpp",
    "include/shadow/image/fused_raw_development.hpp",
    "include/shadow/image/raw_denoise.hpp",
    "include/shadow/image/cxx_bridge.hpp",
    "include/shadow/image/cxx_preview_frame.hpp",
    "include/shadow/image/color_management.hpp",
    "include/shadow/image/source_rendering.hpp",
    "include/shadow/image/source_profile_catalog.hpp",
    "include/shadow/image/warm_edit_preview.hpp",
    "include/shadow/image/working_rgb.hpp",
    "src/bridge/adjustment_render_wire.hpp",
    "src/bridge/cxx_bridge_projection.hpp",
];

const EMBEDDED_IMAGE_SOURCES: &[&str] = &[
    "src/acceleration/image_acceleration_policy.cpp",
    "src/decoder/decoder_error.cpp",
    "src/decoder/decoder_metadata.cpp",
    "src/decoder/decoder_types.cpp",
    "src/decoder/libraw_runtime.cpp",
    "src/decoder/libraw_reference_development.cpp",
    "src/decoder/libraw_decoder.cpp",
    "src/decoder/raster_exif.cpp",
    "src/decoder/raster_decoder.cpp",
    "src/decoder/heif_decoder.cpp",
    "src/decoder/decode_session_isolation.cpp",
    "src/decoder/photo_decoder_router.cpp",
    "src/color/lcms_color_management.cpp",
    "src/color/neutral_balance.cpp",
    "src/color/source_profile_catalog.cpp",
    "src/color/source_rendering.cpp",
    "src/concurrency/row_scheduler.cpp",
    "src/edit/adjustment_graph.cpp",
    "src/edit/adjustment_node_diagnostics.cpp",
    "src/edit/adjustment_execution.cpp",
    "src/edit/cube_lut.cpp",
    "src/edit/cpu_reference.cpp",
    "src/edit/creative_detail_grading.cpp",
    "src/edit/edit_execution_validation.cpp",
    "src/edit/edit_error.cpp",
    "src/edit/finishing_effects_cpu.cpp",
    "src/edit/guided_selective_tone.cpp",
    "src/edit/local_mask.cpp",
    "src/edit/local_mask_coverage.cpp",
    "src/edit/local_mask_validation.cpp",
    "src/edit/metal_adjustment_program.cpp",
    "src/edit/oklab_color_warper.cpp",
    "src/edit/perceptual_color.cpp",
    "src/edit/perceptual_hue_selection.cpp",
    "src/edit/perceptual_contrast.cpp",
    "src/edit/photo_geometry.cpp",
    "src/edit/photo_liquify.cpp",
    "src/edit/photo_structural_rendering.cpp",
    "src/edit/retouch.cpp",
    "src/edit/retouch_heal_blending.cpp",
    "src/edit/scalar_neighborhood_filters.cpp",
    "src/edit/technical_detail_cpu.cpp",
    "src/edit/tone_curve.cpp",
    "src/edit/working_color_math.cpp",
    "src/raw/bayer_demosaic.cpp",
    "src/raw/bayer_sampling.cpp",
    "src/raw/camera_profile_catalog.cpp",
    "src/raw/dcp_color_development.cpp",
    "src/raw/dcp_color_rendering.cpp",
    "src/raw/dcp_parser.cpp",
    "src/raw/fused_raw_development.cpp",
    "src/raw/raw_denoise.cpp",
    "src/raw/raw_denoise_plan.cpp",
    "src/raw/raw_frame_development_plan.cpp",
    "src/raw/raw_frame_region_development.cpp",
    "src/raw/raw_frame_source_preparation.cpp",
    "src/raw/raw_frame_source_development.cpp",
    "src/raw/raw_pipeline.cpp",
    "src/raw/resident_raw_source.cpp",
    "src/raw/sensor_clipping.cpp",
    "src/decoder/private_decoder_plugin.cpp",
    "src/optics/lensfun_cpu_reference.cpp",
    "src/optics/lensfun_profile_catalog.cpp",
    "src/optics/lensfun_modifier_plan.cpp",
    "src/optics/lensfun_region_plan.cpp",
    "src/optics/manual_optics.cpp",
    "src/optics/lensfun_optics.cpp",
    "src/optics/scene_linear_region_optics.cpp",
    "src/proxy/developed_source_raster.cpp",
    "src/proxy/display_output.cpp",
    "src/proxy/edited_proxy_rendering.cpp",
    "src/proxy/edit_preview_frame.cpp",
    "src/proxy/edit_preview_rendering.cpp",
    "src/proxy/full_edit_detail.cpp",
    "src/proxy/full_edit_detail_gpu_cache.cpp",
    "src/proxy/full_edit_detail_gpu_cache_resident.cpp",
    "src/proxy/full_edit_detail_source_preparation.cpp",
    "src/proxy/jpeg_display_luma.cpp",
    "src/proxy/jpeg_proxy_encoding.cpp",
    "src/proxy/proxy_rendering.cpp",
    "src/proxy/proxy_render_request_validation.cpp",
    "src/proxy/warm_edit_gpu_brush_index.cpp",
    "src/proxy/warm_edit_gpu_geometry_plan.cpp",
    "src/proxy/warm_edit_gpu_layer_plan.cpp",
    "src/proxy/warm_edit_gpu_mask_plan.cpp",
    "src/proxy/warm_edit_gpu_neighbourhood_plan.cpp",
    "src/proxy/warm_edit_gpu_retouch_plan.cpp",
    "src/proxy/warm_edit_gpu_render_plan.cpp",
    "src/proxy/warm_edit_preview.cpp",
];

const EMBEDDED_IMAGE_METAL_SOURCES: &[&str] = &[
    "src/edit/metal_adjustment.mm",
    "src/optics/metal_manual_optics.mm",
    "src/optics/metal_scene_linear_region_optics.mm",
    "src/raw/metal_dcp_color_encoding.mm",
    "src/raw/metal_dcp_color_rendering.mm",
    "src/raw/metal_resident_raw_source.mm",
    "src/raw/metal_raw_denoise_encoding.mm",
    "src/raw/metal_raw_denoise.mm",
    "src/raw/metal_raw_reconstruction.mm",
    "src/raw/metal_raw_runtime.mm",
    "src/proxy/full_edit_detail_metal_source.mm",
    "src/proxy/metal_display_output.mm",
    "src/proxy/warm_edit_gpu_dispatcher.mm",
    "src/proxy/warm_edit_gpu_geometry_encoder.mm",
    "src/proxy/warm_edit_gpu_layer_dispatcher.mm",
    "src/proxy/warm_edit_gpu_pipeline_context.mm",
    "src/proxy/warm_edit_gpu_presentation_surface.mm",
    "src/proxy/warm_edit_gpu_resident_resources.mm",
    "src/proxy/warm_edit_gpu_retouch_encoder.mm",
    "src/proxy/warm_edit_gpu_stage_encoder.mm",
    "src/proxy/warm_edit_gpu_transaction.mm",
    "src/proxy/warm_edit_gpu_transaction_encoder.mm",
    "src/proxy/warm_edit_gpu.mm",
];

const EMBEDDED_IMAGE_STUB_SOURCES: &[&str] = &[
    "src/edit/metal_adjustment_stub.cpp",
    "src/optics/metal_manual_optics_stub.cpp",
    "src/optics/metal_scene_linear_region_optics_stub.cpp",
    "src/raw/metal_resident_raw_source_stub.cpp",
    "src/raw/metal_raw_development_stub.cpp",
    "src/proxy/full_edit_detail_metal_source_stub.cpp",
    "src/proxy/metal_display_output_stub.cpp",
    "src/proxy/warm_edit_gpu_presentation_surface_stub.cpp",
    "src/proxy/warm_edit_gpu_stub.cpp",
];

const EMBEDDED_IMAGE_ADDITIONAL_INPUTS: &[&str] = &[
    "include/shadow/image/neutral_balance.hpp",
    "src/acceleration/image_acceleration_policy.hpp",
    "src/concurrency/row_scheduler.hpp",
    "src/decoder/decode_session_isolation.hpp",
    "src/decoder/heif_decoder.hpp",
    "src/decoder/libraw_reference_development.hpp",
    "src/decoder/libraw_runtime.hpp",
    "src/decoder/raster_exif.hpp",
    "src/edit/adjustment_node_diagnostics.hpp",
    "src/edit/creative_detail_grading.hpp",
    "src/edit/edit_execution_validation.hpp",
    "src/edit/finishing_effects_cpu.hpp",
    "src/edit/guided_selective_tone.hpp",
    "src/edit/local_mask_coverage.hpp",
    "src/edit/local_mask_validation.hpp",
    "src/edit/metal_adjustment_execution.hpp",
    "src/edit/metal_adjustment_msl.hpp",
    "src/edit/metal_adjustment_program.hpp",
    "src/edit/oklab_color_warper.hpp",
    "src/edit/perceptual_color.hpp",
    "src/edit/perceptual_hue_selection.hpp",
    "src/edit/perceptual_contrast.hpp",
    "src/edit/photo_geometry_sampling.hpp",
    "src/edit/rgb_pixel_traversal.hpp",
    "src/edit/retouch_heal_blending.hpp",
    "src/edit/scalar_neighborhood_filters.hpp",
    "src/edit/technical_detail_cpu.hpp",
    "src/edit/tone_curve_internal.hpp",
    "src/edit/working_color_math.hpp",
    "src/optics/lensfun_modifier_plan.hpp",
    "src/optics/lensfun_modifier_plan_internal.hpp",
    "src/optics/lensfun_profile_catalog.hpp",
    "src/optics/manual_optics.hpp",
    "src/optics/metal_manual_optics.hpp",
    "src/optics/metal_scene_linear_region_optics.hpp",
    "src/optics/metal_scene_linear_region_optics_msl.hpp",
    "src/optics/scene_linear_region_optics.hpp",
    "src/raw/raw_frame_region_development.hpp",
    "src/raw/raw_frame_source_preparation.hpp",
    "src/raw/resident_raw_source.hpp",
    "src/proxy/developed_source_raster.hpp",
    "src/proxy/display_rgb_math.hpp",
    "src/proxy/edit_preview_rendering.hpp",
    "src/proxy/full_edit_detail_gpu_cache.hpp",
    "src/proxy/full_edit_detail_metal_source.hpp",
    "src/proxy/full_edit_detail_metal_source_msl.hpp",
    "src/proxy/full_edit_detail_source_preparation.hpp",
    "src/proxy/jpeg_proxy_encoding.hpp",
    "src/proxy/metal_display_output.hpp",
    "src/proxy/proxy_render_request_validation.hpp",
    "src/proxy/warm_edit_gpu_brush_index.hpp",
    "src/proxy/warm_edit_gpu_color_matrix.hpp",
    "src/proxy/warm_edit_gpu.hpp",
    "src/proxy/warm_edit_gpu_dispatcher.hpp",
    "src/proxy/warm_edit_gpu_geometry_encoder.hpp",
    "src/proxy/warm_edit_gpu_geometry_msl.hpp",
    "src/proxy/warm_edit_gpu_geometry_plan.hpp",
    "src/proxy/warm_edit_gpu_kernel_contract.hpp",
    "src/proxy/warm_edit_gpu_layer_dispatcher.hpp",
    "src/proxy/warm_edit_gpu_layer_plan.hpp",
    "src/proxy/warm_edit_gpu_mask_msl.hpp",
    "src/proxy/warm_edit_gpu_mask_plan.hpp",
    "src/proxy/warm_edit_gpu_msl.hpp",
    "src/proxy/warm_edit_gpu_neighbourhood_plan.hpp",
    "src/proxy/warm_edit_gpu_pipeline_context.hpp",
    "src/proxy/warm_edit_gpu_presentation_surface.hpp",
    "src/proxy/warm_edit_gpu_render_plan.hpp",
    "src/proxy/warm_edit_gpu_retouch_encoder.hpp",
    "src/proxy/warm_edit_gpu_retouch_plan.hpp",
    "src/proxy/warm_edit_gpu_resident_resources.hpp",
    "src/proxy/warm_edit_gpu_stage_encoder.hpp",
    "src/proxy/warm_edit_gpu_transaction.hpp",
    "src/proxy/warm_edit_gpu_transaction_encoder.hpp",
    "src/raw/bayer_sampling.hpp",
    "src/raw/dcp_color_matrix_math.hpp",
    "src/raw/dcp_color_rendering.hpp",
    "src/raw/metal_dcp_color_encoding.hpp",
    "src/raw/metal_raw_development.hpp",
    "src/raw/metal_raw_development_msl.hpp",
    "src/raw/metal_raw_denoise_encoding.hpp",
    "src/raw/metal_resident_raw_source.hpp",
    "src/raw/metal_raw_runtime.hpp",
    "src/raw/raw_denoise_plan.hpp",
    "src/raw/raw_frame_development_plan.hpp",
    "src/raw/raw_frame_source_development.hpp",
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

fn track_inputs(image_root: &Path, inputs: &[&str]) {
    for relative_path in inputs {
        println!(
            "cargo:rerun-if-changed={}",
            image_root.join(relative_path).display()
        );
    }
}

fn verify_embedded_image_source_manifest(image_root: &Path) {
    let cmake_path = image_root.join("CMakeLists.txt");
    let cmake = fs::read_to_string(&cmake_path)
        .unwrap_or_else(|error| panic!("failed to read {}: {error}", cmake_path.display()));
    let cmake_sources = cmake
        .split_whitespace()
        .map(|token| token.trim_matches(|character| matches!(character, '"' | '(' | ')')))
        .filter(|token| {
            token.starts_with("src/")
                && Path::new(token)
                    .extension()
                    .and_then(|extension| extension.to_str())
                    .is_some_and(|extension| {
                        extension.eq_ignore_ascii_case("cpp")
                            || extension.eq_ignore_ascii_case("mm")
                    })
        })
        .map(ToOwned::to_owned)
        .collect::<BTreeSet<_>>();
    let cargo_sources = EMBEDDED_IMAGE_SOURCES
        .iter()
        .chain(EMBEDDED_IMAGE_METAL_SOURCES)
        .chain(EMBEDDED_IMAGE_STUB_SOURCES)
        .map(|path| (*path).to_owned())
        .collect::<BTreeSet<_>>();
    let missing = cmake_sources.difference(&cargo_sources).collect::<Vec<_>>();
    let extra = cargo_sources.difference(&cmake_sources).collect::<Vec<_>>();
    assert!(
        missing.is_empty() && extra.is_empty(),
        "direct Cargo shadow-image source manifest diverged from CMake; missing={missing:?}; extra={extra:?}"
    );
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
    verify_embedded_image_source_manifest(&image_root);
    println!(
        "cargo:rerun-if-changed={}",
        image_root.join("CMakeLists.txt").display()
    );
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
    for relative_path in BRIDGE_SOURCES {
        build.file(image_root.join(relative_path));
    }
    build.include(&image_include).std("c++20");

    if external_image {
        // CMake already owns Shadow::Image for desktop builds. Keep only the generated CXX glue
        // and the coarse bridge shim in Cargo's archive; the final Qt target resolves their native
        // calls through the single CMake-built image library.
        configure_warnings!(&mut build);
        build.compile("shadow-bridge-cxx");
        println!("cargo:rerun-if-changed=src/lib.rs");
        track_inputs(&image_root, BRIDGE_SOURCES);
        track_inputs(&image_root, BRIDGE_ADDITIONAL_INPUTS);
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

    for relative_path in EMBEDDED_IMAGE_SOURCES {
        build.file(image_root.join(relative_path));
    }
    if metal_enabled {
        for relative_path in EMBEDDED_IMAGE_METAL_SOURCES {
            build.file(image_root.join(relative_path));
        }
        build.define("SHADOW_IMAGE_HAS_METAL", Some("1"));
    } else {
        for relative_path in EMBEDDED_IMAGE_STUB_SOURCES {
            build.file(image_root.join(relative_path));
        }
        build.define("SHADOW_IMAGE_HAS_METAL", Some("0"));
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
    track_inputs(&image_root, BRIDGE_SOURCES);
    track_inputs(&image_root, BRIDGE_ADDITIONAL_INPUTS);
    track_inputs(&image_root, EMBEDDED_IMAGE_SOURCES);
    track_inputs(&image_root, EMBEDDED_IMAGE_METAL_SOURCES);
    track_inputs(&image_root, EMBEDDED_IMAGE_STUB_SOURCES);
    track_inputs(&image_root, EMBEDDED_IMAGE_ADDITIONAL_INPUTS);
}
