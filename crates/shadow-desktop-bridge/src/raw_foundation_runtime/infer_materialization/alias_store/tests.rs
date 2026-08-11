use std::fs;

use uuid::Uuid;

use shadow_cache::FoundationArtifactVerification;

use super::{InferCacheAliasExpectation, InferCacheAliasStore};

struct FixtureRoot(std::path::PathBuf);

impl FixtureRoot {
    fn new() -> Self {
        let path = std::env::temp_dir().join(format!(
            "shadow-raw-foundation-alias-test-{}",
            Uuid::now_v7()
        ));
        fs::create_dir(&path).unwrap();
        Self(path)
    }
}

impl Drop for FixtureRoot {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

#[test]
fn missing_or_malformed_alias_is_a_clean_pre_infer_cache_miss() {
    let root = FixtureRoot::new();
    let store = shadow_cache::FoundationArtifactStore::open(root.0.join("store")).unwrap();
    let aliases = InferCacheAliasStore::new(store.root());
    let expected = expectation();
    assert!(aliases.lookup(&store, &expected).unwrap().is_none());

    let path = aliases.alias_path(expected.source_revision);
    fs::create_dir_all(path.parent().unwrap()).unwrap();
    fs::write(&path, b"not-json\n").unwrap();
    assert!(aliases.lookup(&store, &expected).unwrap().is_none());
}

#[test]
fn records_one_owner_only_alias_only_for_matching_verified_provenance() {
    let root = FixtureRoot::new();
    let store = shadow_cache::FoundationArtifactStore::open(root.0.join("store")).unwrap();
    let aliases = InferCacheAliasStore::new(store.root());
    let expected = expectation();
    let verification = verification(&expected);

    aliases.record(&expected, &verification).unwrap();

    let path = aliases.alias_path(expected.source_revision);
    let text = fs::read_to_string(&path).unwrap();
    assert!(text.contains("shadow.raw-foundation-cache-alias@20260812.1"));
    assert!(aliases.lookup(&store, &expected).unwrap().is_none());
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt as _;

        assert_eq!(
            fs::metadata(path).unwrap().permissions().mode() & 0o777,
            0o600
        );
    }
}

fn expectation() -> InferCacheAliasExpectation<'static> {
    InferCacheAliasExpectation {
        source_revision: "shadow:raw-foundation/source:abc/staging:def",
        source_sha256: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        source_size_bytes: 42,
        decoded_samples_sha256: "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
        source_pixel_contract_sha256: "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
        model_package_sha256: "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd",
        model_graph_sha256: "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee",
        implementation_revision: "rawnind-public-bayer-foundation-ort127-exp1",
    }
}

fn verification(expected: &InferCacheAliasExpectation<'_>) -> FoundationArtifactVerification {
    FoundationArtifactVerification {
        path: std::path::PathBuf::from("/not-opened-by-record"),
        width: 8,
        height: 8,
        force_rggb_crop_sensor: [0, 0],
        source_sha256: expected.source_sha256.into(),
        source_size_bytes: expected.source_size_bytes,
        source_pixel_contract_sha256: expected.source_pixel_contract_sha256.into(),
        model_package_sha256: expected.model_package_sha256.into(),
        model_graph_sha256: expected.model_graph_sha256.into(),
        implementation_revision: expected.implementation_revision.into(),
        cache_key_sha256: "1111111111111111111111111111111111111111111111111111111111111111".into(),
        artifact_identity_sha256:
            "2222222222222222222222222222222222222222222222222222222222222222".into(),
        sequence_sha256: "3333333333333333333333333333333333333333333333333333333333333333".into(),
        payload_bytes: 8 * 8 * 3 * 4,
        file_bytes: 4_096,
        file_sha256: "4444444444444444444444444444444444444444444444444444444444444444".into(),
        stripes: Vec::new(),
    }
}
