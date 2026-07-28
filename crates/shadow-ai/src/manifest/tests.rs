use super::*;

fn valid_manifest() -> ModelManifest {
    ModelManifest {
        schema_version: 1,
        model_id: "example.embedding.small".into(),
        exact_revision: "r1".into(),
        artifact: ModelArtifact {
            filename: "model.onnx".into(),
            byte_len: 42,
            sha256: "a".repeat(64),
        },
        capabilities: BTreeSet::from([AiCapability::SimilarityEmbedding]),
        format: ModelFormat::Onnx,
        opset: Some(18),
        quantization: Quantization::Float16,
        preprocessing_version: "rgb-224-v1".into(),
        inputs: vec![TensorContract {
            name: "image".into(),
            semantics: TensorSemantics::Image,
            element_type: ElementType::Float32,
            layout: TensorLayout::Nchw,
            shape: vec![
                Dimension::Fixed { value: 1 },
                Dimension::Fixed { value: 3 },
                Dimension::Fixed { value: 224 },
                Dimension::Fixed { value: 224 },
            ],
            color_space: Some("display_srgb".into()),
            numeric_range: Some(NumericRange {
                minimum: 0.0,
                maximum: 1.0,
            }),
        }],
        outputs: vec![TensorContract {
            name: "embedding".into(),
            semantics: TensorSemantics::ImageEmbedding,
            element_type: ElementType::Float32,
            layout: TensorLayout::Nc,
            shape: vec![
                Dimension::Fixed { value: 1 },
                Dimension::Fixed { value: 384 },
            ],
            color_space: None,
            numeric_range: None,
        }],
        execution_targets: vec![BackendRequirement {
            kind: BackendKind::Cpu,
            minimum_runtime_version: None,
            precisions: vec![NumericPrecision::Float32],
        }],
        minimum_ram_bytes: 256,
        recommended_ram_bytes: 512,
        minimum_device_memory_bytes: 0,
        recommended_device_memory_bytes: 0,
        licensing: LicenseTerms {
            code_license: "Apache-2.0".into(),
            weight_license: "Apache-2.0".into(),
            training_data_notes: Some("must be reviewed".into()),
            redistribution: LicensePermission::Allowed,
            commercial_use: LicensePermission::Allowed,
            access: ModelAccess::Open,
            attribution_files: vec!["NOTICE".into()],
        },
        distribution: DistributionTerms {
            bundled_by_default: false,
            automatic_download_allowed: true,
            side_load_allowed: true,
            upstream_url: "https://example.invalid/model".into(),
        },
    }
}

#[test]
fn valid_manifest_round_trips_as_data() {
    let manifest = valid_manifest();
    manifest.validate().expect("valid manifest");
    let json = serde_json::to_string(&manifest).expect("serialize model manifest");
    let decoded: ModelManifest = serde_json::from_str(&json).expect("deserialize model manifest");
    assert_eq!(manifest, decoded);
}

#[test]
fn manifest_refuses_to_bundle_non_redistributable_weights() {
    let mut manifest = valid_manifest();
    manifest.licensing.redistribution = LicensePermission::Prohibited;
    manifest.distribution.bundled_by_default = true;
    assert_eq!(
        manifest.validate(),
        Err(ModelManifestError::BundledWithoutRedistributionPermission)
    );
}

#[test]
fn manifest_rejects_unpinned_or_malformed_artifacts() {
    let mut manifest = valid_manifest();
    manifest.artifact.sha256 = "latest".into();
    assert_eq!(manifest.validate(), Err(ModelManifestError::InvalidSha256));
}
