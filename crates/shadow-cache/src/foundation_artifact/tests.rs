use std::{
    fs::{self, OpenOptions},
    io::{Seek, SeekFrom, Write},
    path::PathBuf,
    sync::atomic::{AtomicU64, Ordering},
};

use super::*;

static FIXTURE_SEQUENCE: AtomicU64 = AtomicU64::new(0);

fn fixture_path(label: &str) -> PathBuf {
    let sequence = FIXTURE_SEQUENCE.fetch_add(1, Ordering::Relaxed);
    let root = std::env::temp_dir().join(format!(
        "shadow-foundation-artifact-{label}-{}-{sequence}",
        std::process::id()
    ));
    fs::create_dir_all(&root).expect("create fixture root");
    root.join("fixture.shadowrawf")
}

fn contract() -> FoundationContract {
    FoundationContract {
        algorithm: FoundationAlgorithm {
            blend_overlap_packed: 48,
            blend_width_packed: 34,
            exact_halo_packed: 31,
            implementation_revision: IMPLEMENTATION_REVISION.to_owned(),
            inference_passes: 2,
            input_channel_order: ["R", "G1", "G2", "B"].map(str::to_owned),
            normalization: "per-cfa-site-black-to-white-range-clipped".to_owned(),
            output_scale: 2,
            output_space: "linear-camera-rgb".to_owned(),
            padding: "numpy-reflect-direct-index".to_owned(),
            pool_alignment_packed: 16,
            scale_policy: "one-global-output-mean-to-input-mean".to_owned(),
            step_packed: 416,
            tile_edge_packed: 512,
            white_balance: "none".to_owned(),
        },
        execution: FoundationExecution {
            active_providers: vec!["CPUExecutionProvider".to_owned()],
            engine: "onnxruntime".to_owned(),
            machine: "arm64".to_owned(),
            platform: "macOS-test".to_owned(),
            requested_provider: "CPUExecutionProvider".to_owned(),
            runtime_version: "test".to_owned(),
        },
        model: FoundationModel {
            graph_member: "rawdenoise-nind/model_bayer.onnx".to_owned(),
            graph_sha256: BAYER_GRAPH_SHA256.to_owned(),
            license: "GPL-3.0".to_owned(),
            package_sha256: PACKAGE_SHA256.to_owned(),
            release: UPSTREAM_RELEASE.to_owned(),
            repository: UPSTREAM_REPOSITORY.to_owned(),
            revision: UPSTREAM_REVISION.to_owned(),
            training_repository: TRAINING_REPOSITORY.to_owned(),
            training_revision: TRAINING_REVISION.to_owned(),
        },
        raw_preprocessing: RawPreprocessing {
            black_level_per_channel: vec![512.0; 4],
            color_description: "RGBG".to_owned(),
            decoded_samples_sha256: "4".repeat(64),
            decoder_provider_id: "shadow.test.raw".to_owned(),
            decoder_provider_version: "1".to_owned(),
            force_rggb_crop_sensor: [0, 0],
            packed_shape: [4, 2, 3],
            raw_pattern: [[0, 1], [3, 2]],
            sensor_shape: [4, 6],
            source_raw_pattern: [[0, 1], [3, 2]],
            white_level: 16_383.0,
        },
        source_sha256: "1".repeat(64),
        source_size_bytes: 4096,
        source_pixel_contract_sha256: SOURCE_PIXEL_CONTRACT_SHA256.to_owned(),
    }
}

fn stripe_bytes(y_start: u32, rows: u32, width: u32) -> Vec<u8> {
    let mut result = Vec::new();
    for channel in 0..3 {
        for local_y in 0..rows {
            for x in 0..width {
                let integer = u16::try_from(channel * 100 + (y_start + local_y) * 10 + x)
                    .expect("fixture value is bounded");
                let value = f32::from(integer);
                result.extend_from_slice(&value.to_le_bytes());
            }
        }
    }
    result
}

fn write_fixture(path: &Path) -> FoundationArtifactVerification {
    let contract = contract();
    let shape = [3, 4, 6];
    let cache_key = canonical_sha256(&CacheKeyMaterial {
        contract: &contract,
        output_shape_sensor: shape,
        schema: CACHE_KEY_SCHEMA,
    })
    .expect("cache key");
    let header = FoundationHeader {
        cache_key_sha256: cache_key.clone(),
        contract,
        output: FoundationOutput {
            byte_order: "little".to_owned(),
            channels: ["R", "G", "B"].map(str::to_owned),
            demosaiced: true,
            dtype: "float32".to_owned(),
            layout: "stripe-chw".to_owned(),
            shape_sensor: shape,
            space: "linear-camera-rgb".to_owned(),
        },
        schema: HEADER_SCHEMA.to_owned(),
        semantic_boundary: "raw-foundation-materialization".to_owned(),
    };
    let header_bytes = canonical_json(&header).expect("header JSON");
    let header_sha256 = hex_digest(sha256_bytes(&header_bytes));
    let mut bytes = Vec::new();
    bytes.extend_from_slice(FILE_MAGIC);
    bytes.extend_from_slice(&(header_bytes.len() as u64).to_le_bytes());
    bytes.extend_from_slice(&header_bytes);

    let sequence_header = canonical_json(&SequenceHeader {
        format: SEQUENCE_FORMAT,
        shape,
    })
    .expect("sequence header");
    let mut sequence_hasher = Sha256::new();
    sequence_hasher.update(&sequence_header);
    let mut stripes = Vec::new();
    for (index, y_start) in [0_u32, 2].into_iter().enumerate() {
        let payload = stripe_bytes(y_start, 2, 6);
        sequence_hasher.update(&payload);
        stripes.push(StripeEntry {
            byte_length: payload.len() as u64,
            index,
            offset: bytes.len() as u64,
            rows: 2,
            sha256: hex_digest(sha256_bytes(&payload)),
            y_start,
        });
        bytes.extend_from_slice(&payload);
    }
    let publication = StripePublication {
        first_pass_raw_output_mean: 0.25,
        global_gain: 2.0,
        global_input_mean: 0.5,
        output_mean: 0.5,
        producer: PUBLICATION_PRODUCER.to_owned(),
        replay_relative_mean_delta: 0.0,
        second_pass_raw_output_mean: 0.25,
    };
    let payload_bytes = stripes.iter().map(|stripe| stripe.byte_length).sum();
    let sequence_sha256 = hex_digest(sequence_hasher.finalize());
    let artifact_identity = canonical_sha256(&ArtifactIdentityMaterial {
        cache_key_sha256: &cache_key,
        header_sha256: &header_sha256,
        payload_bytes,
        publication: &publication,
        schema: ARTIFACT_SCHEMA,
        sequence_sha256: &sequence_sha256,
        stripes: &stripes,
    })
    .expect("artifact identity");
    let manifest = FoundationManifest {
        artifact_identity_sha256: artifact_identity,
        cache_key_sha256: cache_key,
        header_sha256,
        payload_bytes,
        publication,
        schema: ARTIFACT_SCHEMA.to_owned(),
        sequence_sha256,
        stripes,
    };
    let manifest_bytes = canonical_json(&manifest).expect("manifest JSON");
    let manifest_offset = bytes.len() as u64;
    bytes.extend_from_slice(&manifest_bytes);
    bytes.extend_from_slice(FOOTER_MAGIC);
    bytes.extend_from_slice(&manifest_offset.to_le_bytes());
    bytes.extend_from_slice(&(manifest_bytes.len() as u64).to_le_bytes());
    bytes.extend_from_slice(&sha256_bytes(&manifest_bytes));
    fs::write(path, bytes).expect("write synthetic artifact");
    verify_foundation_artifact(path).expect("verify synthetic artifact")
}

#[test]
fn sha256_implementation_matches_standard_vectors() {
    assert_eq!(
        hex_digest(sha256_bytes(b"")),
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
    );
    assert_eq!(
        hex_digest(sha256_bytes(b"abc")),
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
    );
}

#[test]
fn source_file_sha256_streams_the_complete_file() {
    let path = fixture_path("source-sha256");
    fs::write(&path, b"abc").expect("write source fixture");

    assert_eq!(
        sha256_file(&path).expect("source SHA-256"),
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
    );
}

#[test]
fn cache_key_changes_with_the_source_pixel_contract() {
    let original = contract();
    let mut changed = original.clone();
    changed.source_pixel_contract_sha256 = "3".repeat(64);
    let shape = [3, 4, 6];

    let original_key = canonical_sha256(&CacheKeyMaterial {
        contract: &original,
        output_shape_sensor: shape,
        schema: CACHE_KEY_SCHEMA,
    })
    .expect("original cache key");
    let changed_key = canonical_sha256(&CacheKeyMaterial {
        contract: &changed,
        output_shape_sensor: shape,
        schema: CACHE_KEY_SCHEMA,
    })
    .expect("changed cache key");

    assert_ne!(original_key, changed_key);
}

#[test]
fn artifact_v1_magic_is_not_accepted_as_v2() {
    let path = fixture_path("v1-magic");
    write_fixture(&path);
    let mut file = OpenOptions::new()
        .write(true)
        .open(&path)
        .expect("open fixture");
    file.write_all(b"SHRAWF01").expect("replace magic");
    file.sync_all().expect("sync changed magic");

    assert!(matches!(
        verify_foundation_artifact(&path),
        Err(FoundationArtifactError::Invalid("file magic changed"))
    ));
}

#[test]
fn verified_reader_crosses_stripes_without_materializing_the_file() {
    let path = fixture_path("rows");
    let verification = write_fixture(&path);
    assert_eq!((verification.width, verification.height), (6, 4));
    assert_eq!(verification.force_rggb_crop_sensor, [0, 0]);
    assert_eq!(verification.stripes.len(), 2);

    let mut reader = FoundationArtifactReader::open(&path).expect("open verified reader");
    let rows = reader
        .read_interleaved_rows(1, 2)
        .expect("read rows across stripe boundary");
    assert_eq!(rows.len(), 2 * 6 * 3);
    assert_eq!(&rows[..6], &[10.0, 110.0, 210.0, 11.0, 111.0, 211.0]);
    assert_eq!(&rows[18..24], &[20.0, 120.0, 220.0, 21.0, 121.0, 221.0]);
}

#[test]
fn payload_tampering_fails_before_any_rows_are_exposed() {
    let path = fixture_path("tamper");
    let verification = write_fixture(&path);
    let offset = verification.stripes[0].offset;
    let mut file = OpenOptions::new()
        .write(true)
        .open(&path)
        .expect("open fixture for tamper");
    file.seek(SeekFrom::Start(offset)).expect("seek payload");
    file.write_all(&1.0_f32.to_le_bytes())
        .expect("tamper payload");
    file.sync_all().expect("sync tamper");

    assert!(matches!(
        FoundationArtifactReader::open(&path),
        Err(FoundationArtifactError::Invalid(_))
    ));
}

#[test]
fn real_audited_artifact_is_optionally_verified() {
    let Some(path) = std::env::var_os("SHADOW_TEST_FOUNDATION_ARTIFACT") else {
        return;
    };
    let mut reader =
        FoundationArtifactReader::open(PathBuf::from(path)).expect("verify real audited artifact");
    let verification = reader.verification();
    assert_eq!(verification.model_package_sha256, PACKAGE_SHA256);
    assert_eq!(verification.model_graph_sha256, BAYER_GRAPH_SHA256);
    assert_eq!(
        verification.source_pixel_contract_sha256,
        SOURCE_PIXEL_CONTRACT_SHA256
    );
    assert_eq!(
        verification.implementation_revision,
        IMPLEMENTATION_REVISION
    );
    let width = verification.width;
    let rows = reader
        .read_interleaved_rows(540, 36)
        .expect("read real rows across the first stripe boundary");
    assert_eq!(rows.len(), 36 * width as usize * 3);
    assert!(rows.iter().all(|value| value.is_finite()));
}
