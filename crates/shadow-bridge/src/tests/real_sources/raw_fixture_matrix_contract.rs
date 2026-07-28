//! Recursive local-RAW fixture coverage and fallback classification.

use std::path::Path;

use shadow_domain::PreviewCodec;

use crate::{
    extract_best_libraw_preview, inspect_libraw, query_libraw_optics_profiles,
    render_libraw_reference_proxy,
};

#[test]
#[ignore = "requires SHADOW_TEST_RAW_FOLDER to contain local RAW fixtures"]
#[allow(clippy::too_many_lines)] // Keeps the end-to-end local fixture contract in one test.
fn real_raw_folder_smoke_matrix() {
    fn collect_raws(directory: &Path, paths: &mut Vec<std::path::PathBuf>) {
        let entries = std::fs::read_dir(directory)
            .unwrap_or_else(|error| panic!("read RAW fixture directory {directory:?}: {error}"));
        for entry in entries {
            let entry = entry.unwrap_or_else(|error| panic!("read RAW fixture entry: {error}"));
            let path = entry.path();
            if path.is_dir() {
                collect_raws(&path, paths);
                continue;
            }
            let extension = path
                .extension()
                .and_then(std::ffi::OsStr::to_str)
                .unwrap_or_default()
                .to_ascii_lowercase();
            if matches!(
                extension.as_str(),
                "3fr"
                    | "arw"
                    | "cr2"
                    | "cr3"
                    | "dng"
                    | "erf"
                    | "fff"
                    | "iiq"
                    | "kdc"
                    | "mef"
                    | "mos"
                    | "mrw"
                    | "nef"
                    | "nrw"
                    | "orf"
                    | "pef"
                    | "raf"
                    | "raw"
                    | "rw2"
                    | "rwl"
                    | "sr2"
                    | "srf"
                    | "srw"
            ) {
                paths.push(path);
            }
        }
    }

    let folder = std::env::var_os("SHADOW_TEST_RAW_FOLDER")
        .expect("SHADOW_TEST_RAW_FOLDER must identify a fixture directory");
    let folder = Path::new(&folder);
    let mut paths = Vec::new();
    collect_raws(folder, &mut paths);
    paths.sort();
    assert!(
        !paths.is_empty(),
        "RAW fixture directory contains no supported files"
    );

    let mut failures = Vec::new();
    let mut passed = 0_usize;
    let mut preview_only = 0_usize;
    let mut processed_rgb_compatibility = 0_usize;
    for path in paths {
        let result = (|| -> Result<(String, RawSmokePath), String> {
            const FULL_DECODE_UNAVAILABLE: &str = "RAW frame/reference RGB unavailable";
            let snapshot = inspect_libraw(&path).map_err(|error| format!("inspect: {error}"))?;
            if snapshot.provider.id != "libraw" {
                return Err(format!("unexpected provider {}", snapshot.provider.id));
            }
            if !snapshot.capabilities.metadata.is_available() {
                return Err("metadata unavailable".to_owned());
            }
            if snapshot.metadata.raw_dimensions.pixel_count() == 0 {
                return Err("invalid RAW dimensions".to_owned());
            }

            let preview = extract_best_libraw_preview(&path)
                .map_err(|error| format!("extract preview: {error}"))?
                .map(|preview| {
                    if preview.descriptor.dimensions.pixel_count() == 0 || preview.bytes.is_empty()
                    {
                        return Err("invalid embedded preview".to_owned());
                    }
                    Ok(preview)
                })
                .transpose()?;

            let raw_frame_available = snapshot.capabilities.raw_frame.is_available();
            let reference_rgb_available = snapshot.capabilities.reference_rgb.is_available();

            let profile_count = query_libraw_optics_profiles(&path)
                .map_err(|error| format!("query Lensfun profiles: {error}"))?
                .len();
            let summary = format!(
                "{} {} · {}x{} · {profile_count} compatible optical profiles",
                snapshot.metadata.normalized_make,
                snapshot.metadata.normalized_model,
                snapshot.metadata.raw_dimensions.width,
                snapshot.metadata.raw_dimensions.height,
            );
            if !reference_rgb_available {
                if preview.is_none() {
                    return Err(format!("{FULL_DECODE_UNAVAILABLE}; no embedded preview"));
                }
                return Ok((
                    format!("{summary} · embedded-preview fallback"),
                    RawSmokePath::EmbeddedPreview,
                ));
            }

            let proxy = render_libraw_reference_proxy(&path, 1_024, 82)
                .map_err(|error| format!("render reference proxy: {error}"))?;
            if proxy.codec != PreviewCodec::Jpeg {
                return Err(format!("unexpected proxy codec: {:?}", proxy.codec));
            }
            if proxy.dimensions.width.max(proxy.dimensions.height) > 1_024
                || proxy.dimensions.pixel_count() == 0
                || proxy.bytes.is_empty()
            {
                return Err("invalid bounded reference proxy".to_owned());
            }

            let path = if raw_frame_available {
                RawSmokePath::RawFrame
            } else {
                RawSmokePath::ProcessedRgbCompatibility
            };
            Ok((summary, path))
        })();

        match result {
            Ok((summary, path_kind)) => {
                passed += 1;
                match path_kind {
                    RawSmokePath::RawFrame => {
                        eprintln!("RAW smoke ok: {} · {summary}", path.display());
                    }
                    RawSmokePath::ProcessedRgbCompatibility => {
                        processed_rgb_compatibility += 1;
                        eprintln!(
                            "RAW smoke processed-RGB compatibility: {} · {summary}",
                            path.display()
                        );
                    }
                    RawSmokePath::EmbeddedPreview => {
                        preview_only += 1;
                        eprintln!("RAW smoke preview-only: {} · {summary}", path.display());
                    }
                }
            }
            Err(error) => {
                eprintln!("RAW smoke failed: {} · {error}", path.display());
                failures.push(format!("{} · {error}", path.display()));
            }
        }
    }

    assert!(
        failures.is_empty(),
        "RAW smoke matrix: {passed} passed ({preview_only} preview-only), {} failed:\n{}",
        failures.len(),
        failures.join("\n")
    );
    eprintln!(
        "RAW smoke matrix passed: {passed} files · {processed_rgb_compatibility} processed-RGB compatibility · {preview_only} preview-only fallbacks"
    );
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum RawSmokePath {
    RawFrame,
    ProcessedRgbCompatibility,
    EmbeddedPreview,
}
