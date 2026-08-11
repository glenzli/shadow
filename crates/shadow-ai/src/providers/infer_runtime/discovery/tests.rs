use std::{
    fs,
    path::{Path, PathBuf},
    sync::{
        Arc,
        atomic::{AtomicU64, Ordering},
    },
};

use super::*;

static NEXT_FIXTURE: AtomicU64 = AtomicU64::new(1);

struct DiscoveryFixture {
    root: PathBuf,
}

impl DiscoveryFixture {
    fn new() -> Self {
        let ordinal = NEXT_FIXTURE.fetch_add(1, Ordering::Relaxed);
        let root = std::env::temp_dir().join(format!(
            "shadow-infer-discovery-{}-{ordinal}",
            std::process::id()
        ));
        let registrations = root.join("registrations");
        let sockets = root.join("sockets");
        fs::create_dir_all(&registrations).expect("create discovery fixture");
        fs::create_dir_all(&sockets).expect("create discovery socket fixture");
        set_mode(&root, 0o700);
        set_mode(&registrations, 0o700);
        set_mode(&sockets, 0o700);
        Self { root }
    }

    fn manifest(&self) -> PathBuf {
        self.root
            .join("registrations")
            .join("infer-runtime--local.json")
    }

    fn write_registration(&self, generation: &str, endpoint: &str, version: &str) {
        self.write_registration_versions(generation, endpoint, &[version]);
    }

    fn write_registration_versions(&self, generation: &str, endpoint: &str, versions: &[&str]) {
        let document = serde_json::json!({
            "schema": "infra.discovery.registration",
            "schema_version": "20260812.1",
            "service": {
                "kind": "infer-runtime",
                "instance_id": "local",
                "generation": generation
            },
            "offers": [{
                "protocol": "infer-runtime.status",
                "protocol_versions": ["20260810.1"],
                "binding": "infra.local.unix-socket",
                "endpoint": "sockets/example.sock"
            }, {
                "protocol": "infer-runtime.consumer",
                "protocol_versions": versions,
                "binding": "infer-runtime.http-loopback",
                "endpoint": endpoint
            }]
        });
        fs::write(
            self.manifest(),
            serde_json::to_vec_pretty(&document).expect("registration JSON"),
        )
        .expect("write registration");
        set_mode(&self.manifest(), 0o600);
    }
}

impl Drop for DiscoveryFixture {
    fn drop(&mut self) {
        fs::remove_dir_all(&self.root).expect("remove discovery fixture");
    }
}

#[test]
fn selects_the_exact_consumer_offer() {
    let fixture = DiscoveryFixture::new();
    fixture.write_registration(
        "generation-a",
        "http://127.0.0.1:9123",
        CONSUMER_PROTOCOL_VERSION_CANDIDATE_3,
    );

    let endpoint = discover_endpoint(&fixture.root).expect("discover endpoint");
    assert_eq!(endpoint.base_url.as_str(), "http://127.0.0.1:9123/");
    assert_eq!(
        endpoint.consumer_version,
        InferRuntimeConsumerVersion::Candidate3
    );
    assert!(matches!(
        endpoint.source,
        DiscoveryEndpointSource::Discovered {
            ref instance_id,
            ref generation,
            ..
        } if instance_id == "local" && generation == "generation-a"
    ));
}

#[test]
fn selects_candidate_3_when_the_offer_also_lists_an_unsupported_older_version() {
    let fixture = DiscoveryFixture::new();
    fixture.write_registration_versions(
        "generation-a",
        "http://127.0.0.1:9123",
        &["0.1.0-candidate.2", CONSUMER_PROTOCOL_VERSION_CANDIDATE_3],
    );

    let endpoint = discover_endpoint(&fixture.root).expect("discover endpoint");
    assert_eq!(
        endpoint.consumer_version,
        InferRuntimeConsumerVersion::Candidate3
    );
}

#[test]
fn resolver_keeps_registration_as_a_candidate_and_tracks_generation_changes() {
    let fixture = DiscoveryFixture::new();
    fixture.write_registration(
        "generation-a",
        "http://127.0.0.1:9123",
        CONSUMER_PROTOCOL_VERSION_CANDIDATE_3,
    );
    let resolver = InferRuntimeDiscoveryResolver::from_root(fixture.root.clone());
    let first = resolver.resolve();
    assert_eq!(resolver.resolve(), first);

    fixture.write_registration(
        "generation-b",
        "http://127.0.0.1:9456",
        CONSUMER_PROTOCOL_VERSION_CANDIDATE_3,
    );
    let second = resolver.resolve();
    assert_ne!(first, second);
    assert_eq!(second.base_url.as_str(), "http://127.0.0.1:9456/");
    assert_eq!(
        second.consumer_version,
        InferRuntimeConsumerVersion::Candidate3
    );

    assert_eq!(resolver.resolve(), second);
}

#[test]
fn connection_failure_rechecks_generation_before_using_fallback() {
    let fixture = DiscoveryFixture::new();
    fixture.write_registration(
        "generation-a",
        "http://127.0.0.1:9123",
        CONSUMER_PROTOCOL_VERSION_CANDIDATE_3,
    );
    let resolver = InferRuntimeDiscoveryResolver::from_root(fixture.root.clone());
    let failed = resolver.resolve();

    fixture.write_registration(
        "generation-b",
        "http://127.0.0.1:9456",
        CONSUMER_PROTOCOL_VERSION_CANDIDATE_3,
    );
    let rediscovered = resolver.resolve_after_connection_failure(&failed);
    assert_eq!(rediscovered.base_url.as_str(), "http://127.0.0.1:9456/");

    let unchanged = resolver.resolve_after_connection_failure(&rediscovered);
    assert!(matches!(
        unchanged.source,
        DiscoveryEndpointSource::FixedFallback
    ));
}

#[test]
fn typed_client_connection_failure_invokes_rediscovery() {
    let fixture = DiscoveryFixture::new();
    let discovered = "http://127.0.0.1:1";
    let fallback = "http://127.0.0.1:2";
    fixture.write_registration(
        "generation-a",
        discovered,
        CONSUMER_PROTOCOL_VERSION_CANDIDATE_3,
    );
    let resolver = Arc::new(InferRuntimeDiscoveryResolver::from_root_with_fallback(
        fixture.root.clone(),
        fallback,
    ));
    let credential =
        super::super::InferRuntimeCredential::parse(&"a".repeat(64)).expect("test credential");
    let client = super::super::InferRuntimeClient::with_endpoint(
        super::super::InferRuntimeEndpoint::Discovery(Arc::clone(&resolver)),
        credential,
    )
    .expect("discovery client");

    let result: Result<serde_json::Value, _> = client
        .send_json("infer/v1/jobs", |endpoint, _consumer_version| {
            Ok(client.client.get(endpoint))
        });
    assert!(matches!(
        result,
        Err(super::super::InferRuntimeClientError::Request(_))
    ));
    assert!(matches!(
        resolver
            .cached_endpoint()
            .expect("rediscovery updates the cached selection")
            .source,
        DiscoveryEndpointSource::FixedFallback
    ));
}

#[test]
fn unavailable_or_incompatible_discovery_uses_the_migration_fallback() {
    let fixture = DiscoveryFixture::new();
    let resolver = InferRuntimeDiscoveryResolver::from_root(fixture.root.clone());
    let unavailable = resolver.resolve();
    assert!(matches!(
        unavailable.source,
        DiscoveryEndpointSource::FixedFallback
    ));
    assert_eq!(
        unavailable.consumer_version,
        InferRuntimeConsumerVersion::Candidate3
    );

    fixture.write_registration("generation-a", "http://127.0.0.1:9123", "0.1.0-candidate.2");
    let incompatible = resolver.resolve();
    assert!(matches!(
        incompatible.source,
        DiscoveryEndpointSource::FixedFallback
    ));
    assert_eq!(
        incompatible.consumer_version,
        InferRuntimeConsumerVersion::Candidate3
    );
}

#[test]
fn explicit_endpoint_uses_candidate_3_vocabulary() {
    let endpoint = DiscoveryEndpoint::explicit(
        validate_loopback_base_url("http://127.0.0.1:9876").expect("explicit endpoint"),
    );
    assert_eq!(
        endpoint.consumer_version,
        InferRuntimeConsumerVersion::Candidate3
    );
}

#[test]
fn rejects_the_previous_schema_and_the_removed_lease_field() {
    let fixture = DiscoveryFixture::new();
    fixture.write_registration(
        "generation-a",
        "http://127.0.0.1:9123",
        CONSUMER_PROTOCOL_VERSION_CANDIDATE_3,
    );
    let mut registration: serde_json::Value =
        serde_json::from_slice(&fs::read(fixture.manifest()).expect("read registration"))
            .expect("parse registration");

    registration["schema_version"] = serde_json::json!("20260810.1");
    fs::write(
        fixture.manifest(),
        serde_json::to_vec_pretty(&registration).expect("previous registration JSON"),
    )
    .expect("write previous registration");
    set_mode(&fixture.manifest(), 0o600);
    assert!(matches!(
        discover_endpoint(&fixture.root),
        Err(InferRuntimeDiscoveryError::InvalidRegistration)
    ));

    registration["schema_version"] = serde_json::json!("20260812.1");
    registration["lease"] = serde_json::json!({
        "renewed_at": "2026-08-12T00:00:00Z",
        "expires_at": "2026-08-12T00:01:00Z"
    });
    fs::write(
        fixture.manifest(),
        serde_json::to_vec_pretty(&registration).expect("lease-bearing registration JSON"),
    )
    .expect("write lease-bearing registration");
    set_mode(&fixture.manifest(), 0o600);
    assert!(matches!(
        discover_endpoint(&fixture.root),
        Err(InferRuntimeDiscoveryError::InvalidJson(_))
    ));
}

#[test]
fn rejects_noncanonical_endpoint_and_duplicate_or_unknown_fields() {
    let fixture = DiscoveryFixture::new();
    fixture.write_registration(
        "generation-a",
        "http://localhost:8787",
        CONSUMER_PROTOCOL_VERSION_CANDIDATE_3,
    );
    assert!(matches!(
        discover_endpoint(&fixture.root),
        Err(InferRuntimeDiscoveryError::InvalidEndpoint)
    ));

    let manifest = fs::read_to_string(fixture.manifest()).expect("read registration");
    let duplicate = manifest.replacen(
        "\"schema\": \"infra.discovery.registration\"",
        "\"schema\": \"infra.discovery.registration\", \"schema\": \"infra.discovery.registration\"",
        1,
    );
    fs::write(fixture.manifest(), duplicate).expect("write duplicate field");
    set_mode(&fixture.manifest(), 0o600);
    assert!(matches!(
        discover_endpoint(&fixture.root),
        Err(InferRuntimeDiscoveryError::InvalidJson(_))
    ));
}

#[cfg(unix)]
#[test]
fn rejects_registration_that_is_not_owner_only() {
    let fixture = DiscoveryFixture::new();
    fixture.write_registration(
        "generation-a",
        "http://127.0.0.1:9123",
        CONSUMER_PROTOCOL_VERSION_CANDIDATE_3,
    );
    set_mode(&fixture.manifest(), 0o644);
    assert!(matches!(
        discover_endpoint(&fixture.root),
        Err(InferRuntimeDiscoveryError::UnsafeObject(_))
    ));
}

#[cfg(unix)]
fn set_mode(path: &Path, mode: u32) {
    use std::os::unix::fs::PermissionsExt;

    fs::set_permissions(path, fs::Permissions::from_mode(mode)).expect("set fixture permissions");
}

#[cfg(windows)]
fn set_mode(_path: &Path, _mode: u32) {}
