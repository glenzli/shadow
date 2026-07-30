use super::*;

fn artifact_set(artifacts: Vec<ModelArtifact>) -> ModelArtifactSet {
    let mut set = ModelArtifactSet {
        set_id: "example.embedding.small-r1".into(),
        inventory_blake3: String::new(),
        artifacts,
    };
    set.inventory_blake3 = set
        .computed_inventory_blake3()
        .expect("valid artifact fixture");
    set
}

fn refresh_artifact_set_identity(manifest: &mut ModelManifest) {
    manifest.artifact_set.inventory_blake3 = manifest
        .artifact_set
        .computed_inventory_blake3()
        .expect("valid changed artifact fixture");
}

fn valid_manifest() -> ModelManifest {
    ModelManifest {
        schema_version: 1,
        model_id: "example.embedding.small".into(),
        exact_revision: "r1".into(),
        artifact_set: artifact_set(vec![ModelArtifact {
            relative_path: "model.onnx".into(),
            role: ModelArtifactRole::ModelDefinition,
            byte_len: 42,
            sha256: "a".repeat(64),
        }]),
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
    manifest.artifact_set.artifacts[0].sha256 = "latest".into();
    assert_eq!(manifest.validate(), Err(ModelManifestError::InvalidSha256));
}

#[test]
fn manifest_supports_one_exact_multi_package_core_ml_set() {
    let mut manifest = valid_manifest();
    manifest.format = ModelFormat::CoreMlPackage;
    manifest.opset = None;
    manifest.artifact_set.artifacts = vec![
        ModelArtifact {
            relative_path: "image-encoder.mlpackage.zip".into(),
            role: ModelArtifactRole::CoreMlPackageArchive,
            byte_len: 21,
            sha256: "1".repeat(64),
        },
        ModelArtifact {
            relative_path: "prompt-encoder.mlpackage.zip".into(),
            role: ModelArtifactRole::CoreMlPackageArchive,
            byte_len: 22,
            sha256: "2".repeat(64),
        },
        ModelArtifact {
            relative_path: "mask-decoder.mlpackage.zip".into(),
            role: ModelArtifactRole::CoreMlPackageArchive,
            byte_len: 23,
            sha256: "3".repeat(64),
        },
    ];
    refresh_artifact_set_identity(&mut manifest);

    manifest.validate().expect("valid Core ML artifact set");
}

#[test]
fn manifest_supports_an_exact_extracted_core_ml_package_inventory() {
    let mut manifest = valid_manifest();
    manifest.format = ModelFormat::CoreMlPackage;
    manifest.opset = None;
    manifest.artifact_set.artifacts = vec![
        ModelArtifact {
            relative_path: "encoder.mlpackage/Manifest.json".into(),
            role: ModelArtifactRole::CoreMlPackageMember,
            byte_len: 21,
            sha256: "1".repeat(64),
        },
        ModelArtifact {
            relative_path: "encoder.mlpackage/Data/com.apple.CoreML/model.mlmodel".into(),
            role: ModelArtifactRole::CoreMlPackageMember,
            byte_len: 22,
            sha256: "2".repeat(64),
        },
    ];
    refresh_artifact_set_identity(&mut manifest);

    manifest
        .validate()
        .expect("valid extracted Core ML artifact set");
}

#[test]
fn core_ml_manifest_cannot_relabel_an_unrelated_artifact_set() {
    let mut manifest = valid_manifest();
    manifest.format = ModelFormat::CoreMlPackage;
    manifest.opset = None;

    assert_eq!(
        manifest.validate(),
        Err(ModelManifestError::MissingCoreMlPackageArtifact)
    );
}

#[test]
fn manifest_rejects_path_traversal_and_duplicate_artifacts() {
    let mut traversal = valid_manifest();
    traversal.artifact_set.artifacts[0].relative_path = "../model.onnx".into();
    assert!(matches!(
        traversal.validate(),
        Err(ModelManifestError::InvalidArtifactPath(_))
    ));

    let mut duplicate = valid_manifest();
    duplicate
        .artifact_set
        .artifacts
        .push(duplicate.artifact_set.artifacts[0].clone());
    assert!(matches!(
        duplicate.validate(),
        Err(ModelManifestError::DuplicateArtifactPath(_))
    ));
}

#[test]
fn portable_artifact_paths_reject_platform_and_case_aliases_before_hashing() {
    for path in [
        "C:/model.onnx",
        "//server/model.onnx",
        r"\\server\model.onnx",
        "weights/model:stream",
        "weights/CON.txt",
        "weights/LPT9.bin",
        "weights/model.",
        "weights/model ",
        " weights/model.onnx",
        "weights/ model.onnx",
        "weights/model~1.onnx",
        "weights/~model.onnx",
        "weights/mo\u{0301}del.onnx",
    ] {
        let mut manifest = valid_manifest();
        manifest.artifact_set.artifacts[0].relative_path = path.into();
        assert!(
            matches!(
                manifest.validate(),
                Err(ModelManifestError::InvalidArtifactPath(ref actual)) if actual == path
            ),
            "{path:?} must fail before inventory hashing"
        );
    }

    let mut aliases = valid_manifest();
    aliases.artifact_set.artifacts = vec![
        ModelArtifact {
            relative_path: "Weights/model.onnx".into(),
            role: ModelArtifactRole::ModelDefinition,
            byte_len: 42,
            sha256: "a".repeat(64),
        },
        ModelArtifact {
            relative_path: "weights/MODEL.onnx".into(),
            role: ModelArtifactRole::Weights,
            byte_len: 43,
            sha256: "b".repeat(64),
        },
    ];
    assert!(matches!(
        aliases.validate(),
        Err(ModelManifestError::ArtifactPathAlias { .. })
    ));

    let mut attribution_aliases = valid_manifest();
    attribution_aliases.licensing.attribution_files =
        vec!["legal/NOTICE".into(), "LEGAL/notice".into()];
    assert!(matches!(
        attribution_aliases.validate(),
        Err(ModelManifestError::ArtifactPathAlias { .. })
    ));
}

#[test]
fn artifact_inventory_rejects_short_name_and_leading_space_aliases_before_hashing() {
    for path in [
        " weights/model.onnx",
        "weights/ model.onnx",
        "weights/model~1.onnx",
        "weights/~model.onnx",
    ] {
        let mut artifact_set = valid_manifest().artifact_set;
        artifact_set.artifacts[0].relative_path = path.into();
        artifact_set.artifacts[0].sha256 = "not-a-digest".into();

        assert!(
            matches!(
                artifact_set.computed_inventory_blake3(),
                Err(ModelManifestError::InvalidArtifactPath(ref actual)) if actual == path
            ),
            "{path:?} must fail path validation before inventory hashing"
        );
    }
}

#[test]
fn manifest_wire_is_exact_recursively_and_rejects_duplicate_sets() {
    let manifest = valid_manifest();
    let value = serde_json::to_value(&manifest).expect("serialize manifest");

    let mut unsupported = value.clone();
    unsupported["schema_version"] = serde_json::json!(MODEL_MANIFEST_SCHEMA_VERSION + 1);
    assert!(serde_json::from_value::<ModelManifest>(unsupported).is_err());

    let mut unknown_tensor_fact = value.clone();
    unknown_tensor_fact["inputs"][0]["implicit_resize"] = serde_json::json!(true);
    assert!(serde_json::from_value::<ModelManifest>(unknown_tensor_fact).is_err());

    let mut duplicate_capability = value;
    let capability = duplicate_capability["capabilities"][0].clone();
    duplicate_capability["capabilities"]
        .as_array_mut()
        .expect("capability array")
        .push(capability);
    assert!(serde_json::from_value::<ModelManifest>(duplicate_capability).is_err());
}

#[test]
fn manifest_nested_vectors_are_stream_bounded() {
    let manifest = valid_manifest();
    let mut oversized_shape = serde_json::to_value(&manifest).expect("serialize manifest");
    oversized_shape["inputs"][0]["shape"] = serde_json::Value::Array(vec![
        serde_json::json!({
            "kind": "fixed",
            "value": 1
        });
        17
    ]);
    assert!(serde_json::from_value::<ModelManifest>(oversized_shape).is_err());

    let mut oversized_inputs = serde_json::to_value(&manifest).expect("serialize manifest");
    oversized_inputs["inputs"] =
        serde_json::Value::Array(vec![oversized_inputs["inputs"][0].clone(); 65]);
    assert!(serde_json::from_value::<ModelManifest>(oversized_inputs).is_err());

    let mut oversized_artifacts = serde_json::to_value(&manifest).expect("serialize manifest");
    oversized_artifacts["artifact_set"]["artifacts"] = serde_json::Value::Array(vec![
        oversized_artifacts["artifact_set"]["artifacts"][0].clone();
        129
    ]);
    assert!(serde_json::from_value::<ModelManifest>(oversized_artifacts).is_err());
}

#[test]
fn artifact_set_identity_is_order_independent_but_inventory_sensitive() {
    let first = ModelArtifact {
        relative_path: "encoder/model.onnx".into(),
        role: ModelArtifactRole::ModelDefinition,
        byte_len: 11,
        sha256: "1".repeat(64),
    };
    let second = ModelArtifact {
        relative_path: "encoder/weights.bin".into(),
        role: ModelArtifactRole::Weights,
        byte_len: 22,
        sha256: "2".repeat(64),
    };
    let forward = artifact_set(vec![first.clone(), second.clone()]);
    let reverse = artifact_set(vec![second, first]);
    assert_eq!(forward.inventory_blake3, reverse.inventory_blake3);
    assert_eq!(
        forward.inventory_blake3,
        "261b066dcc142b366c069689fe7bb5d95af62cc9eedd02aa00ff47f8dd89e87c"
    );

    let mut changed_path = forward.clone();
    changed_path.artifacts[0].relative_path = "decoder/model.onnx".into();
    let mut changed_role = forward.clone();
    changed_role.artifacts[0].role = ModelArtifactRole::Auxiliary;
    let mut changed_length = forward.clone();
    changed_length.artifacts[0].byte_len += 1;
    let mut changed_hash = forward.clone();
    changed_hash.artifacts[0].sha256 = "3".repeat(64);
    for changed in [changed_path, changed_role, changed_length, changed_hash] {
        assert_ne!(
            forward.inventory_blake3,
            changed
                .computed_inventory_blake3()
                .expect("changed identity")
        );
    }
}

#[test]
fn manifest_rejects_a_spoofed_artifact_set_identity() {
    let mut manifest = valid_manifest();
    let expected = manifest.artifact_set.inventory_blake3.clone();
    manifest.artifact_set.inventory_blake3 = "0".repeat(64);
    assert_eq!(
        manifest.validate(),
        Err(ModelManifestError::ArtifactSetIdentityMismatch {
            expected,
            actual: "0".repeat(64),
        })
    );
}
