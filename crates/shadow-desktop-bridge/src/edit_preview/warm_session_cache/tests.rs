use std::{
    path::{Path, PathBuf},
    sync::{
        Arc, Barrier,
        atomic::{AtomicUsize, Ordering},
    },
    thread,
};

use anyhow::{Result as AnyResult, anyhow};
use shadow_ai::{
    ArtifactHashAlgorithm, GeneratedArtifactReference, RAW_FOUNDATION_ENCODING_VERSION,
    RAW_FOUNDATION_MEDIA_TYPE, RasterExtent, RawFoundationArtifact, RawFoundationProvenance,
    RawFoundationSourceProvenance,
};
use shadow_domain::{EntityId, RawFoundationDenoiseModel, RawTemperatureTint, RawWhiteBalance};

use super::*;
use crate::raw_foundation_runtime::RawFoundationReady;

const DISPLAY_JPEG_BYTES: &[u8] = &[
    0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 0x4a, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01,
    0x00, 0x01, 0x00, 0x00, 0xff, 0xdb, 0x00, 0x43, 0x00, 0x03, 0x02, 0x02, 0x03, 0x02, 0x02, 0x03,
    0x03, 0x03, 0x03, 0x04, 0x03, 0x03, 0x04, 0x05, 0x08, 0x05, 0x05, 0x04, 0x04, 0x05, 0x0a, 0x07,
    0x07, 0x06, 0x08, 0x0c, 0x0a, 0x0c, 0x0c, 0x0b, 0x0a, 0x0b, 0x0b, 0x0d, 0x0e, 0x12, 0x10, 0x0d,
    0x0e, 0x11, 0x0e, 0x0b, 0x0b, 0x10, 0x16, 0x10, 0x11, 0x13, 0x14, 0x15, 0x15, 0x15, 0x0c, 0x0f,
    0x17, 0x18, 0x16, 0x14, 0x18, 0x12, 0x14, 0x15, 0x14, 0xff, 0xc0, 0x00, 0x0b, 0x08, 0x00, 0x02,
    0x00, 0x02, 0x01, 0x01, 0x11, 0x00, 0xff, 0xc4, 0x00, 0x14, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0xff, 0xc4, 0x00, 0x1d,
    0x10, 0x00, 0x02, 0x01, 0x04, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x01, 0x02, 0x06, 0x03, 0x04, 0x05, 0x07, 0x00, 0x12, 0x62, 0xff, 0xda, 0x00, 0x08, 0x01,
    0x01, 0x00, 0x00, 0x3f, 0x00, 0x41, 0xe2, 0xfa, 0x1b, 0x59, 0xd3, 0x8d, 0x62, 0x55, 0x75, 0xdc,
    0x4d, 0x55, 0x6d, 0x28, 0x80, 0xa3, 0x09, 0x6c, 0x00, 0x1d, 0x07, 0x8e, 0x7f, 0xff, 0xd9,
];

fn fixture_root(name: &str) -> PathBuf {
    let root = std::env::temp_dir().join(format!(
        "shadow-warm-preview-cache-{name}-{}-{}",
        std::process::id(),
        RepresentationId::new_v7()
    ));
    std::fs::create_dir_all(&root).expect("create warm-preview fixture root");
    root
}

fn write_display_jpeg(path: &Path) {
    std::fs::write(path, DISPLAY_JPEG_BYTES).expect("write display JPEG fixture");
}

fn open_fixture_session(path: &Path) -> AnyResult<PhotoEditPreviewSession> {
    PhotoEditPreviewSession::open(path, 64).map_err(Into::into)
}

fn key() -> WarmEditPreviewSessionKey {
    WarmEditPreviewSessionKey {
        representation_id: RepresentationId::new_v7(),
        source: RepresentationFingerprint {
            byte_len: DISPLAY_JPEG_BYTES.len() as u64,
            modified_at_ms: Some(1_000),
        },
        max_edge: 64,
        source_environment_cache_identity: "source-environment-v1-fixture".to_owned(),
        requested_raw_development_plan_identity: "shadow-raw-plan-v1;fixture=preview".to_owned(),
        raw_development_plan: RawDevelopmentPlan::preview(),
        optics: OpticsSettings::default(),
        raw_foundation: None,
        raw_foundation_amount_percent: None,
    }
}

fn raw_foundation_identity() -> RawFoundationRenderIdentity {
    let ready = RawFoundationReady {
        descriptor: RawFoundationArtifact::new(
            GeneratedArtifactReference::new(
                ArtifactHashAlgorithm::Sha256,
                "1".repeat(64),
                4_096,
                RAW_FOUNDATION_MEDIA_TYPE.into(),
                RAW_FOUNDATION_ENCODING_VERSION,
            )
            .expect("artifact reference"),
            RasterExtent::new(6, 4).expect("extent"),
            RawFoundationSourceProvenance::new("2".repeat(64), 8_192, "3".repeat(64))
                .expect("source provenance"),
            RawFoundationProvenance::new(
                "4".repeat(64),
                "5".repeat(64),
                RawFoundationDenoiseModel::RAWNIND_PACKAGE_SHA256.into(),
                RawFoundationDenoiseModel::RAWNIND_BAYER_GRAPH_SHA256.into(),
                RawFoundationDenoiseModel::RAWNIND_IMPLEMENTATION_REVISION.into(),
            )
            .expect("foundation provenance"),
        )
        .expect("foundation descriptor"),
        path: PathBuf::from("/cache/foundation.shadowrawf"),
        source_path: PathBuf::from("/source/input.nef"),
        source: RepresentationFingerprint {
            byte_len: 8_192,
            modified_at_ms: Some(17),
        },
        disposition: shadow_ai::RawFoundationMaterializationDisposition::Published,
        verified_reader: None,
        raw_frame_staging: None,
    };
    RawFoundationRenderIdentity::from_ready(
        &ready,
        RawFoundationDenoiseModel::RawNindPublicBayerRelease5_6_0,
    )
    .expect("render identity")
}

#[test]
fn identical_full_key_returns_the_same_prepared_session() {
    let root = fixture_root("same-key");
    let source = root.join("source.jpg");
    write_display_jpeg(&source);
    let cache = WarmEditPreviewSessionCache::default();
    let prepare_count = AtomicUsize::new(0);
    let requested = key();

    let first = cache
        .get_or_prepare_with(requested.clone(), || {
            prepare_count.fetch_add(1, Ordering::SeqCst);
            open_fixture_session(&source)
        })
        .expect("prepare cold session");
    let second = cache
        .get_or_prepare_with(requested, || {
            prepare_count.fetch_add(1, Ordering::SeqCst);
            open_fixture_session(&source)
        })
        .expect("reuse warm session");

    assert!(Arc::ptr_eq(&first, &second));
    assert_eq!(prepare_count.load(Ordering::SeqCst), 1);
    assert_eq!(cache.entries.lock().expect("cache entries").len(), 1);
    std::fs::remove_dir_all(root).expect("remove warm-preview fixture");
}

#[test]
fn only_white_balance_can_share_a_rebinding_source() {
    let base = key();
    let mut white_balance_variant = base.clone();
    white_balance_variant.raw_development_plan =
        RawDevelopmentPlan::preview().with_white_balance(RawWhiteBalance::temperature_tint(
            RawTemperatureTint::new(6_800, 18).expect("manual temperature/tint"),
        ));
    white_balance_variant
        .requested_raw_development_plan_identity
        .push_str("-manual-white-balance");
    assert!(!base.matches(&white_balance_variant));
    assert!(base.shares_rebindable_raw_source(&white_balance_variant));

    let mut sensor_stage_variant = white_balance_variant;
    sensor_stage_variant.raw_development_plan.noise_reduction =
        shadow_bridge::RawNoiseReductionIntent::NoiseRobust;
    assert!(!base.shares_rebindable_raw_source(&sensor_stage_variant));
}

#[test]
fn raw_white_balance_picker_reuses_the_same_cfa_source_across_preview_edges() {
    let base = key();
    let mut another_preview_edge = base.clone();
    another_preview_edge.max_edge = 1_536;
    assert!(
        !base.matches(&another_preview_edge),
        "different presentation edges remain distinct render-cache entries"
    );
    assert!(
        !base.shares_rebindable_raw_source(&another_preview_edge),
        "render rebinding keeps its bounded presentation edge"
    );
    assert!(
        base.shares_raw_white_balance_picker_source(&another_preview_edge),
        "the picker samples normalized CFA coordinates and must not fail after a preview-size change"
    );

    let mut changed_sensor_stage = another_preview_edge;
    changed_sensor_stage.raw_development_plan.noise_reduction =
        shadow_bridge::RawNoiseReductionIntent::NoiseRobust;
    assert!(
        !base.shares_raw_white_balance_picker_source(&changed_sensor_stage),
        "a picker may never cross a RAW sensor-stage change"
    );
}

#[test]
fn persisted_manual_white_balance_prepares_an_as_shot_resident_source() {
    let requested =
        RawDevelopmentPlan::preview().with_white_balance(RawWhiteBalance::temperature_tint(
            RawTemperatureTint::new(7_350, -71).expect("manual temperature/tint"),
        ));

    let base = manual_white_balance_base_plan(requested)
        .expect("manual white balance requires an as-shot resident preparation");
    assert!(base.white_balance.is_as_shot());
    assert_eq!(base.noise_reduction, requested.noise_reduction);
    assert_eq!(base.quality, requested.quality);
    assert_eq!(base.intent, requested.intent);
    assert!(manual_white_balance_base_plan(RawDevelopmentPlan::preview()).is_none());
}

#[test]
fn foundation_amount_is_an_exact_output_key_but_not_a_cold_source_key() {
    let mut full = key();
    full.raw_foundation = Some(raw_foundation_identity());
    full.raw_foundation_amount_percent = Some(100);
    let mut partial = full.clone();
    partial.raw_foundation_amount_percent = Some(37);

    assert!(!full.matches(&partial));
    assert!(
        full.shares_rebindable_raw_source(&partial),
        "amount changes must share the verified artifact and paired bounded camera basis"
    );
}

#[test]
fn every_prepared_source_key_component_participates_in_reuse() {
    let root = fixture_root("key-components");
    let source = root.join("source.jpg");
    write_display_jpeg(&source);
    let cache = WarmEditPreviewSessionCache::default();
    let prepare_count = AtomicUsize::new(0);
    let base = key();
    let mut variants = Vec::new();

    let mut changed = base.clone();
    changed.representation_id = RepresentationId::new_v7();
    variants.push(changed);
    let mut changed = base.clone();
    changed.source.byte_len += 1;
    variants.push(changed);
    let mut changed = base.clone();
    changed.source.modified_at_ms = Some(2_000);
    variants.push(changed);
    let mut changed = base.clone();
    changed.max_edge += 1;
    variants.push(changed);
    let mut changed = base.clone();
    changed
        .source_environment_cache_identity
        .push_str("-changed");
    variants.push(changed);
    let mut changed = base.clone();
    changed
        .requested_raw_development_plan_identity
        .push_str("-changed");
    variants.push(changed);
    let mut changed = base.clone();
    changed.optics.manual_distortion = 1;
    variants.push(changed);
    let mut changed = base.clone();
    changed.raw_foundation = Some(raw_foundation_identity());
    changed.raw_foundation_amount_percent = Some(100);
    variants.push(changed);

    cache
        .get_or_prepare_with(base, || {
            prepare_count.fetch_add(1, Ordering::SeqCst);
            open_fixture_session(&source)
        })
        .expect("prepare base session");
    for variant in variants {
        cache
            .get_or_prepare_with(variant, || {
                prepare_count.fetch_add(1, Ordering::SeqCst);
                open_fixture_session(&source)
            })
            .expect("prepare changed-key session");
    }

    assert_eq!(prepare_count.load(Ordering::SeqCst), 9);
    assert_eq!(
        cache.entries.lock().expect("cache entries").len(),
        MAX_WARM_EDIT_PREVIEW_SESSIONS
    );
    std::fs::remove_dir_all(root).expect("remove warm-preview fixture");
}

#[test]
fn warm_hits_promote_mru_and_the_third_cold_source_evicts_lru() {
    let root = fixture_root("lru");
    let source = root.join("source.jpg");
    write_display_jpeg(&source);
    let cache = WarmEditPreviewSessionCache::default();
    let first = key();
    let mut second = key();
    second.representation_id = RepresentationId::new_v7();
    let mut third = key();
    third.representation_id = RepresentationId::new_v7();

    for requested in [first.clone(), second.clone()] {
        cache
            .get_or_prepare_with(requested, || open_fixture_session(&source))
            .expect("prepare cache entry");
    }
    cache
        .get_or_prepare_with(first.clone(), || panic!("MRU hit must not prepare"))
        .expect("promote first entry");
    cache
        .get_or_prepare_with(third.clone(), || open_fixture_session(&source))
        .expect("prepare third entry");

    let entries = cache.entries.lock().expect("cache entries");
    assert_eq!(entries.len(), MAX_WARM_EDIT_PREVIEW_SESSIONS);
    assert!(entries[0].key.matches(&third));
    assert!(entries[1].key.matches(&first));
    assert!(!entries.iter().any(|entry| entry.key.matches(&second)));
    drop(entries);
    std::fs::remove_dir_all(root).expect("remove warm-preview fixture");
}

#[test]
fn concurrent_cold_misses_converge_on_one_cached_arc() {
    let root = fixture_root("concurrent");
    let source = root.join("source.jpg");
    write_display_jpeg(&source);
    let cache = Arc::new(WarmEditPreviewSessionCache::default());
    let barrier = Arc::new(Barrier::new(2));
    let prepare_count = Arc::new(AtomicUsize::new(0));
    let requested = key();

    let (left, right) = thread::scope(|scope| {
        let left_cache = Arc::clone(&cache);
        let left_barrier = Arc::clone(&barrier);
        let left_count = Arc::clone(&prepare_count);
        let left_source = source.clone();
        let left_key = requested.clone();
        let left = scope.spawn(move || {
            left_cache.get_or_prepare_with(left_key, || {
                left_count.fetch_add(1, Ordering::SeqCst);
                let session = open_fixture_session(&left_source)?;
                left_barrier.wait();
                Ok(session)
            })
        });

        let right_cache = Arc::clone(&cache);
        let right_barrier = Arc::clone(&barrier);
        let right_count = Arc::clone(&prepare_count);
        let right_source = source.clone();
        let right = scope.spawn(move || {
            right_cache.get_or_prepare_with(requested, || {
                right_count.fetch_add(1, Ordering::SeqCst);
                let session = open_fixture_session(&right_source)?;
                right_barrier.wait();
                Ok(session)
            })
        });

        (
            left.join().expect("join left preparation"),
            right.join().expect("join right preparation"),
        )
    });
    let left = left.expect("left session");
    let right = right.expect("right session");

    assert_eq!(prepare_count.load(Ordering::SeqCst), 2);
    assert!(Arc::ptr_eq(&left, &right));
    assert_eq!(cache.entries.lock().expect("cache entries").len(), 1);
    std::fs::remove_dir_all(root).expect("remove warm-preview fixture");
}

#[test]
fn cache_mutex_is_not_held_across_preparation() {
    let root = fixture_root("lock-scope");
    let source = root.join("source.jpg");
    write_display_jpeg(&source);
    let cache = WarmEditPreviewSessionCache::default();

    cache
        .get_or_prepare_with(key(), || {
            assert!(
                cache.entries.try_lock().is_ok(),
                "cache container must be unlocked during decode preparation"
            );
            open_fixture_session(&source)
        })
        .expect("prepare outside cache lock");

    std::fs::remove_dir_all(root).expect("remove warm-preview fixture");
}

#[test]
fn poisoned_cache_lock_keeps_the_existing_error_contract() {
    let cache = Arc::new(WarmEditPreviewSessionCache::default());
    let poison = Arc::clone(&cache);
    let _ = thread::spawn(move || {
        let _guard = poison
            .entries
            .lock()
            .expect("lock cache for poison fixture");
        panic!("poison warm-preview cache");
    })
    .join();

    let error = cache
        .get_or_prepare_with(key(), || {
            panic!("poisoned cache must fail before preparation")
        })
        .expect_err("poisoned cache error");
    assert_eq!(error.to_string(), CACHE_LOCK_POISONED);
}

#[test]
fn public_failure_uses_isolated_raster_then_removes_it() {
    let root = fixture_root("fallback-success");
    let source = root.join("source.raw");
    let temporary_raster = root.join("isolated.jpg");
    let mut opened = Vec::new();

    let prepared = prepare_preview_session_with_routes(
        &root,
        &source,
        64,
        RawDevelopmentPlan::preview(),
        &OpticsSettings::default(),
        |path, max_edge, plan, optics| {
            opened.push(path.to_owned());
            if path == source {
                Err(anyhow!("public fixture decoder failure"))
            } else {
                PhotoEditPreviewSession::open_with_raw_development_plan_and_optics(
                    path, max_edge, plan, optics,
                )
                .map_err(Into::into)
            }
        },
        |cache_root, requested_source, max_edge| {
            assert_eq!(cache_root, root);
            assert_eq!(requested_source, source);
            assert_eq!(max_edge, 64);
            write_display_jpeg(&temporary_raster);
            Ok(temporary_raster.clone())
        },
    )
    .expect("prepare isolated raster through public path");

    assert_eq!(prepared.dimensions().width, 2);
    assert_eq!(opened, [source, temporary_raster.clone()]);
    assert!(!temporary_raster.exists());
    std::fs::remove_dir_all(root).expect("remove warm-preview fixture");
}

#[test]
fn failed_isolated_raster_open_is_cleaned_and_retains_public_error() {
    let root = fixture_root("fallback-failure");
    let source = root.join("source.raw");
    let temporary_raster = root.join("isolated.jpg");

    let error = prepare_preview_session_with_routes(
        &root,
        &source,
        64,
        RawDevelopmentPlan::preview(),
        &OpticsSettings::default(),
        |path, _, _, _| -> AnyResult<PhotoEditPreviewSession> {
            if path == source {
                Err(anyhow!("original public decoder failure"))
            } else {
                Err(anyhow!("isolated JPEG public-open failure"))
            }
        },
        |_, _, _| {
            write_display_jpeg(&temporary_raster);
            Ok(temporary_raster.clone())
        },
    )
    .expect_err("both routes must fail");

    let message = error.to_string();
    assert!(message.contains(&source.display().to_string()));
    assert!(message.contains("original public decoder failure"));
    assert!(!temporary_raster.exists());
    std::fs::remove_dir_all(root).expect("remove warm-preview fixture");
}

#[test]
fn manual_foundation_white_balance_never_enters_rgb_isolation() {
    let root = fixture_root("manual-white-balance-no-rgb-fallback");
    let source = root.join("source.raw");
    let plan = RawDevelopmentPlan::preview().with_white_balance(RawWhiteBalance::temperature_tint(
        RawTemperatureTint::new(6_300, 11).expect("manual temperature/tint"),
    ));
    let isolate_count = AtomicUsize::new(0);

    let error = prepare_preview_session_with_routes(
        &root,
        &source,
        64,
        plan,
        &OpticsSettings::default(),
        |_, _, _, _| Err(anyhow!("public RawFrame route unavailable")),
        |_, _, _| {
            isolate_count.fetch_add(1, Ordering::SeqCst);
            Err(anyhow!("manual white balance must stop before isolation"))
        },
    )
    .expect_err("manual RAW white balance must fail closed");

    assert_eq!(isolate_count.load(Ordering::SeqCst), 0);
    assert!(
        error
            .to_string()
            .contains("cannot use an isolated provider-processed RGB fallback")
    );
    std::fs::remove_dir_all(root).expect("remove warm-preview fixture");
}
