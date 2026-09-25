use super::*;

fn unit(value: f64) -> UnitInterval {
    UnitInterval::new(value).expect("valid unit value")
}

fn proposal_artifact() -> GeneratedArtifactReference {
    GeneratedArtifactReference::new(
        ArtifactHashAlgorithm::Blake3_256,
        "a".repeat(64),
        1024,
        "application/x-shadow-soft-mask".to_owned(),
        1,
    )
    .expect("valid proposal artifact")
}

#[test]
fn point_mask_requires_a_foreground_prompt() {
    let prompt = MaskPrompt::Points {
        points: vec![MaskPromptPoint {
            x: unit(0.5),
            y: unit(0.5),
            polarity: MaskPointPolarity::Background,
        }],
    };

    assert_eq!(
        prompt.validate(),
        Err(AiArtifactContractError::MaskHasNoForegroundPoint)
    );
}

#[test]
fn generated_payload_must_match_the_job_kind() {
    let payload = AiGeneratedPayload::SoftMask(SoftMaskArtifact {
        artifact: proposal_artifact(),
        raster_extent: RasterExtent::new(256, 256).expect("valid raster"),
        coordinate_extent: RasterExtent::new(6000, 4000).expect("valid extent"),
        coordinate_space: MaskCoordinateSpace::Original,
        encoding: SoftMaskEncoding::Gray8Unorm,
        semantic: MaskSemantic::Subject,
    });

    assert!(payload.validate_for(AiTaskKind::ProposeSubjectMask).is_ok());
    assert!(matches!(
        payload.validate_for(AiTaskKind::Denoise),
        Err(AiArtifactContractError::TaskPayloadMismatch { .. })
    ));
}

#[test]
fn denoise_never_silently_changes_the_pixel_domain() {
    let output = DenoisedRasterArtifact {
        artifact: proposal_artifact(),
        input_domain: ImageDomain::SensorMosaic,
        output_domain: ImageDomain::SceneLinearRgb,
        raster_extent: RasterExtent::new(6000, 4000).expect("valid extent"),
        pixel_layout: RasterPixelLayout::SensorMosaic,
        sample_format: RasterSampleFormat::Float16,
        source_pixel_contract_hash: "b".repeat(64),
        tile_contract: Some(TileContract {
            tile_edge: 512,
            halo: 32,
        }),
        full_resolution: true,
    };

    assert!(matches!(
        output.validate(),
        Err(AiArtifactContractError::DenoiseChangesImageDomain { .. })
    ));
}

#[test]
fn task_parameters_reject_cross_task_reuse() {
    let parameters = AiTaskParameters::SubjectMask(SubjectMaskParameters {
        prompt: MaskPrompt::AutomaticSubject,
        coordinate_space: MaskCoordinateSpace::Original,
        coordinate_extent: RasterExtent::new(6000, 4000).expect("valid extent"),
        edge_refinement: unit(0.5),
        maximum_candidates: 3,
    });

    assert!(
        parameters
            .validate_for(AiTaskKind::ProposeSubjectMask)
            .is_ok()
    );
    assert!(matches!(
        parameters.validate_for(AiTaskKind::Denoise),
        Err(AiArtifactContractError::TaskParameterMismatch { .. })
    ));
}

#[test]
fn raw_foundation_requires_its_explicit_parameter_marker() {
    assert!(
        AiTaskParameters::RawFoundation
            .validate_for(AiTaskKind::MaterializeRawFoundation)
            .is_ok()
    );
    assert!(matches!(
        AiTaskParameters::Denoise(DenoiseParameters {
            domain_policy: DenoiseDomainPolicy::SensorMosaicRequired,
            quality: DenoiseQuality::Final,
            strength: unit(1.0),
            detail_protection: unit(0.5),
            chroma_reduction: unit(0.5),
        })
        .validate_for(AiTaskKind::MaterializeRawFoundation),
        Err(AiArtifactContractError::TaskParameterMismatch { .. })
    ));
}

#[test]
fn completion_patch_encoding_binds_float_byte_extent_and_version() {
    let mut patch = ImageCompletionPatchArtifact {
        artifact: GeneratedArtifactReference::new(
            ArtifactHashAlgorithm::Blake3_256,
            "a".repeat(64),
            16,
            "application/x-shadow-linear-rgba-f32".into(),
            1,
        )
        .unwrap(),
        raster_extent: RasterExtent::new(1, 1).unwrap(),
        coordinate_extent: RasterExtent::new(100, 100).unwrap(),
        source_recipe_blake3: "b".repeat(64),
        source_context: None,
        provider: "local".into(),
        deployment: "lama".into(),
        model_build: "v1".into(),
        postprocessing_identity: "linear-v2".into(),
        api_contract_revision: "v1".into(),
        actual_execution_provider: "cpu".into(),
    };
    assert!(patch.linear_rgba_f32());
    assert!(patch.validate().is_ok());
    patch.artifact = GeneratedArtifactReference::new(
        ArtifactHashAlgorithm::Blake3_256,
        "a".repeat(64),
        4,
        "application/x-shadow-linear-rgba-f32".into(),
        1,
    )
    .unwrap();
    assert!(patch.validate().is_err());
    patch.artifact = GeneratedArtifactReference::new(
        ArtifactHashAlgorithm::Blake3_256,
        "a".repeat(64),
        16,
        "application/x-shadow-linear-rgba-f32".into(),
        2,
    )
    .unwrap();
    assert!(patch.validate().is_err());
}
