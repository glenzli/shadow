use std::path::PathBuf;

use shadow_catalog::RegisterAsset;
use shadow_core::{fingerprint_source, native_location};
use shadow_domain::{EntityId, RepresentationId, RepresentationKind};

use crate::open_desktop_session;

use super::*;

#[test]
fn known_child_crash_blocks_export_before_a_native_session_can_open() {
    let error = reject_known_quarantined_raw_export(NativeDecodeAdmission::Quarantined {
        observation: crate::isolated_proxy::IsolatedDecodeObservation::ChildCrashed,
    })
    .expect_err("a known child crash must stop the native export opener");
    assert!(error.to_string().contains("temporarily unavailable"));
    assert!(error.to_string().contains("child decoder crashed"));
    assert!(reject_known_quarantined_raw_export(NativeDecodeAdmission::NotQuarantined).is_ok());
}

#[test]
#[ignore = "requires SHADOW_TEST_EXPORT_RAW to name a local RAW fixture"]
fn real_raw_export_renders_a_tightly_packed_full_resolution_raster() {
    let source_path =
        PathBuf::from(std::env::var_os("SHADOW_TEST_EXPORT_RAW").expect("fixture path"));
    let source = fingerprint_source(&source_path).expect("fingerprint export fixture");
    let root = std::env::temp_dir().join(format!(
        "shadow-export-smoke-{}-{}",
        std::process::id(),
        RepresentationId::new_v7()
    ));
    std::fs::create_dir_all(&root).expect("create export smoke root");
    let session = open_desktop_session(
        root.join("catalog.sqlite").to_str().expect("catalog path"),
        root.join("cache").to_str().expect("cache path"),
    )
    .expect("open export smoke session");
    let registered = session
        .catalog
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: native_location(&source_path),
            byte_len: source.byte_len,
            modified_at_ms: source.modified_at_ms,
            now_ms: 1,
        })
        .expect("register export fixture");
    let photo_id = registered.photo_id.to_string();
    let source_path = source_path.to_str().expect("UTF-8 fixture path");
    let state = session
        .photo_edit_state(&photo_id, source_path)
        .expect("prepare neutral working Recipe");
    let raster = session
        .render_basic_edit_export(
            &photo_id,
            source_path,
            &ffi::FfiEditExportRequest {
                base_commit_id: state.working_commit_id,
                settings: state.settings,
                use_working_recipe: true,
                bit_depth: 8,
            },
        )
        .expect("render full-resolution export");

    assert!(raster.width > 0);
    assert!(raster.height > 0);
    assert_eq!(raster.bit_depth, 8);
    assert_eq!(raster.row_stride_bytes, raster.width * 3);
    assert!(raster.samples16.is_empty());
    assert_eq!(
        raster.bytes.len(),
        usize::try_from(u64::from(raster.row_stride_bytes) * u64::from(raster.height))
            .expect("export byte length")
    );

    drop(session);
    std::fs::remove_dir_all(root).expect("remove export smoke root");
}

#[test]
fn rgb16_tile_copy_uses_sample_strides_and_exact_offsets() {
    let rect = DetailTileRect {
        x: 1,
        y: 1,
        width: 2,
        height: 2,
    };
    let source = vec![
        101_u16, 102, 103, 111, 112, 113, 201, 202, 203, 211, 212, 213,
    ];
    let mut destination = vec![0_u16; 4 * 3 * 3];
    copy_export_tile16(
        &mut destination,
        4 * 3,
        &ExportTileCopy16 {
            origin_x: rect.x,
            origin_y: rect.y,
            rect,
            source: &source,
            source_row_stride_bytes: 2 * 3 * 2,
        },
    )
    .expect("copy packed RGB16 tile");
    assert_eq!(&destination[15..21], &source[0..6]);
    assert_eq!(&destination[27..33], &source[6..12]);
    assert!(destination[..15].iter().all(|sample| *sample == 0));
}
