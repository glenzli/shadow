use super::*;

fn unit(value: f64) -> UnitInterval {
    UnitInterval::new(value).expect("valid unit value")
}

fn proposal_artifact() -> GeneratedArtifactReference {
    GeneratedArtifactReference {
        contract_version: AI_GENERATED_ARTIFACT_CONTRACT_VERSION,
        hash_algorithm: ArtifactHashAlgorithm::Blake3_256,
        content_hash: "a".repeat(64),
        byte_len: 1024,
        media_type: "application/x-shadow-soft-mask".to_owned(),
        encoding_version: 1,
        storage_class: GeneratedArtifactStorageClass::RebuildableProposal,
    }
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
