use super::*;
use shadow_ai::{RasterExtent, VisionProvenance};
fn evidence(value: u8) -> InferImageCompletionEvidence {
    InferImageCompletionEvidence {
        rgb8: vec![value; 512 * 512 * 3],
        raster_extent: RasterExtent::new(512, 512).unwrap(),
        input_extent: RasterExtent::new(512, 512).unwrap(),
        provenance: VisionProvenance {
            job_id: "original-job".into(),
            provider: "onnx-local".into(),
            deployment: "lama".into(),
            model_build: "fixed-build".into(),
            artifact_sha256: "a".repeat(64),
            preprocessing_identity: "input".into(),
            postprocessing_identity: "output".into(),
            tokenizer: None,
            runtime: "runtime".into(),
            requested_execution_provider: "cpu".into(),
            actual_execution_provider: "cpu".into(),
            execution_provider_fallback_reason: None,
            precision: "fp32".into(),
        },
    }
}
#[test]
fn bounded_reuse_keeps_original_provenance_and_evicts_least_recent_candidate() {
    let cache = CompletionRequestCache::default();
    for i in 0..8 {
        cache.insert([i; 32], evidence(i));
    }
    assert_eq!(
        cache.get(&[0; 32]).unwrap().provenance.job_id,
        "original-job"
    );
    cache.insert([8; 32], evidence(8));
    assert!(cache.get(&[1; 32]).is_none());
    assert_eq!(cache.get(&[0; 32]).unwrap().rgb8[0], 0);
    cache.insert([0; 32], evidence(22));
    assert_eq!(cache.get(&[0; 32]).unwrap().rgb8[0], 22);
    assert_eq!(cache.entries.lock().unwrap().len(), 8);
}
#[test]
fn key_binds_exact_inputs_and_source_but_not_job_tokens() {
    let mut i = ImageCompletionInvocation {
        request_id: "one".into(),
        promotion_id: "one".into(),
        generation: 1,
        photo_id: "p".into(),
        prepared_crop_png: vec![1],
        prepared_mask_png: vec![2],
        prepared_mask_gray8: vec![255],
        coordinate_extent: RasterExtent::new(20, 30).unwrap(),
        source_recipe_blake3: "a".repeat(64),
        source_context: None,
        mask_revision: "b".repeat(64),
        scene_referred_input: true,
        force_regenerate: false,
    };
    let k = CompletionRequestCache::key(&i);
    i.generation = 2;
    i.request_id = "two".into();
    assert_eq!(k, CompletionRequestCache::key(&i));
    i.prepared_mask_gray8[0] = 0;
    assert_ne!(k, CompletionRequestCache::key(&i));
    i.prepared_mask_gray8[0] = 255;
    i.source_recipe_blake3 = "c".repeat(64);
    assert_ne!(k, CompletionRequestCache::key(&i));
}

#[test]
fn cached_completion_stages_without_runtime_and_promotes_durable_float_bytes() {
    use crate::image_completion_runtime::{ImageCompletionRuntime, ImageCompletionRuntimeError};
    use crate::image_completion_service::{
        ImageCompletionCompletion, ImageCompletionPlacement, ImageCompletionService,
    };
    use shadow_domain::UnitInterval;
    use std::io::{Cursor, Read};
    let root = tempfile::tempdir().unwrap();
    let runtime = ImageCompletionRuntime::new(
        root.path().join("scratch"),
        None,
        root.path().join("missing-credential"),
    )
    .unwrap();
    let service = ImageCompletionService::open(root.path().join("store")).unwrap();
    let mut png = Cursor::new(Vec::new());
    image::RgbImage::from_pixel(512, 512, image::Rgb([250; 3]))
        .write_to(&mut png, image::ImageFormat::Png)
        .unwrap();
    let mask = vec![255; 512 * 512];
    let mut mask_png = Cursor::new(Vec::new());
    image::GrayImage::from_raw(512, 512, mask.clone())
        .unwrap()
        .write_to(&mut mask_png, image::ImageFormat::Png)
        .unwrap();
    let invocation = |force| ImageCompletionInvocation {
        request_id: "cache-stage".into(),
        promotion_id: "cache-promotion".into(),
        generation: 7,
        photo_id: uuid::Uuid::new_v4().to_string(),
        prepared_crop_png: png.get_ref().clone(),
        prepared_mask_png: mask_png.get_ref().clone(),
        prepared_mask_gray8: mask.clone(),
        coordinate_extent: RasterExtent::new(512, 512).unwrap(),
        source_recipe_blake3: "b".repeat(64),
        source_context: None,
        mask_revision: blake3::hash(&mask).to_hex().to_string(),
        scene_referred_input: true,
        force_regenerate: force,
    };
    let mut input = invocation(false);
    input.source_context = Some(shadow_domain::ImageCompletionSourceContext {
        color_basis: Some(shadow_domain::ImageCompletionColorBasis {
            matrix_bits: [1., 0., 0., 0., 1., 0., 0., 0., 1.].map(f64::to_bits),
            calibration_id: "fixture-camera".into(),
        }),
        foundation: Default::default(),
        raw_ai_denoise: Default::default(),
    });
    let expected_context = input.source_context.clone();
    let photo_id = input.photo_id.clone();
    runtime
        .request_cache
        .insert(CompletionRequestCache::key(&input), evidence(250));
    let job = service.begin_job().unwrap();
    let receipt = runtime
        .stage(service.store(), input, &service.cancellation(job).unwrap())
        .unwrap();
    let result = service
        .complete_job(
            job,
            receipt,
            ImageCompletionPlacement {
                bounds_left: UnitInterval::ZERO,
                bounds_top: UnitInterval::ZERO,
                bounds_right: UnitInterval::ONE,
                bounds_bottom: UnitInterval::ONE,
            },
        )
        .unwrap();
    let ImageCompletionCompletion::Staged {
        proposal_token,
        generation,
    } = result
    else {
        panic!("candidate was not staged");
    };
    let preview = service.proposal_preview(proposal_token).unwrap();
    assert!(preview.linear_rgba_f32);
    assert_eq!(preview.rgba8.len(), 512 * 512 * 16);
    let accepted = service
        .promote_proposal(proposal_token, generation)
        .unwrap();
    assert!(accepted.patch().linear_rgba_f32());
    assert_eq!(accepted.patch().source_context(), expected_context.as_ref());
    assert_eq!(
        preview.source_color_basis,
        expected_context
            .as_ref()
            .and_then(|c| c.color_basis.clone())
    );
    let reopened = ImageCompletionService::open(root.path().join("store")).unwrap();
    let mut actual = Vec::new();
    reopened
        .store()
        .open_recipe_completion_patch(accepted.patch())
        .unwrap()
        .read_to_end(&mut actual)
        .unwrap();
    assert_eq!(actual, preview.rgba8);
    assert_eq!(
        std::fs::read_dir(root.path().join("scratch"))
            .unwrap()
            .count(),
        0
    );
    let mut refresh = invocation(true);
    refresh.photo_id = photo_id;
    assert!(
        matches!(
            runtime.stage(
                service.store(),
                refresh,
                &shadow_ai::CancellationToken::default()
            ),
            Err(ImageCompletionRuntimeError::Infer(_))
        ),
        "explicit refresh must use Runtime even when its old candidate is cached"
    );
}
