use std::{
    fs,
    path::{Path, PathBuf},
    sync::{
        Arc,
        atomic::{AtomicU64, Ordering},
    },
};

use time::{Duration, OffsetDateTime, format_description::well_known::Rfc3339};

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

    fn write_registration(
        &self,
        now: OffsetDateTime,
        generation: &str,
        endpoint: &str,
        version: &str,
    ) {
        let renewed_at = (now - Duration::seconds(5))
            .format(&Rfc3339)
            .expect("renewed time");
        let expires_at = (now + Duration::seconds(40))
            .format(&Rfc3339)
            .expect("expiration time");
        let document = serde_json::json!({
            "schema": "infra.discovery.registration",
            "schema_version": "20260810.1",
            "service": {
                "kind": "infer-runtime",
                "instance_id": "local",
                "generation": generation
            },
            "lease": {
                "renewed_at": renewed_at,
                "expires_at": expires_at
            },
            "offers": [{
                "protocol": "infer-runtime.status",
                "protocol_versions": ["20260810.1"],
                "binding": "infra.local.unix-socket",
                "endpoint": "sockets/example.sock"
            }, {
                "protocol": "infer-runtime.consumer",
                "protocol_versions": [version],
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
fn selects_the_exact_live_consumer_offer() {
    let fixture = DiscoveryFixture::new();
    let now = OffsetDateTime::now_utc();
    fixture.write_registration(
        now,
        "generation-a",
        "http://127.0.0.1:9123",
        CONSUMER_PROTOCOL_VERSION,
    );

    let endpoint = discover_endpoint(&fixture.root, now).expect("discover endpoint");
    assert_eq!(endpoint.base_url.as_str(), "http://127.0.0.1:9123/");
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
fn resolver_tracks_generation_changes_and_lease_expiration() {
    let fixture = DiscoveryFixture::new();
    let now = OffsetDateTime::now_utc();
    fixture.write_registration(
        now,
        "generation-a",
        "http://127.0.0.1:9123",
        CONSUMER_PROTOCOL_VERSION,
    );
    let resolver = InferRuntimeDiscoveryResolver::from_root(fixture.root.clone());
    let first = resolver.resolve_at(now);

    fixture.write_registration(
        now,
        "generation-b",
        "http://127.0.0.1:9456",
        CONSUMER_PROTOCOL_VERSION,
    );
    let second = resolver.resolve_at(now);
    assert_ne!(first, second);
    assert_eq!(second.base_url.as_str(), "http://127.0.0.1:9456/");

    let expired = resolver.resolve_at(now + Duration::seconds(121));
    assert_eq!(expired.base_url.as_str(), "http://127.0.0.1:8787/");
    assert!(matches!(
        expired.source,
        DiscoveryEndpointSource::CompatibilityFallback
    ));
}

#[test]
fn connection_failure_rechecks_generation_before_using_fallback() {
    let fixture = DiscoveryFixture::new();
    let now = OffsetDateTime::now_utc();
    fixture.write_registration(
        now,
        "generation-a",
        "http://127.0.0.1:9123",
        CONSUMER_PROTOCOL_VERSION,
    );
    let resolver = InferRuntimeDiscoveryResolver::from_root(fixture.root.clone());
    let failed = resolver.resolve_at(now);

    fixture.write_registration(
        OffsetDateTime::now_utc(),
        "generation-b",
        "http://127.0.0.1:9456",
        CONSUMER_PROTOCOL_VERSION,
    );
    let rediscovered = resolver.resolve_after_connection_failure(&failed);
    assert_eq!(rediscovered.base_url.as_str(), "http://127.0.0.1:9456/");

    let unchanged = resolver.resolve_after_connection_failure(&rediscovered);
    assert!(matches!(
        unchanged.source,
        DiscoveryEndpointSource::CompatibilityFallback
    ));
}

#[test]
fn typed_client_connection_failure_invokes_rediscovery() {
    let fixture = DiscoveryFixture::new();
    let now = OffsetDateTime::now_utc();
    let discovered = "http://127.0.0.1:1";
    let fallback = "http://127.0.0.1:2";
    fixture.write_registration(now, "generation-a", discovered, CONSUMER_PROTOCOL_VERSION);
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

    let result: Result<serde_json::Value, _> =
        client.send_json("infer/v1/jobs", |endpoint| Ok(client.client.get(endpoint)));
    assert!(matches!(
        result,
        Err(super::super::InferRuntimeClientError::Request(_))
    ));
    assert!(matches!(
        resolver
            .cached_endpoint()
            .expect("rediscovery updates the cached selection")
            .source,
        DiscoveryEndpointSource::CompatibilityFallback
    ));
}

#[test]
fn unavailable_or_incompatible_discovery_uses_the_migration_fallback() {
    let fixture = DiscoveryFixture::new();
    let resolver = InferRuntimeDiscoveryResolver::from_root(fixture.root.clone());
    let now = OffsetDateTime::now_utc();
    assert!(matches!(
        resolver.resolve_at(now).source,
        DiscoveryEndpointSource::CompatibilityFallback
    ));

    fixture.write_registration(
        now,
        "generation-a",
        "http://127.0.0.1:9123",
        "0.1.0-candidate.1",
    );
    assert!(matches!(
        resolver.resolve_at(now).source,
        DiscoveryEndpointSource::CompatibilityFallback
    ));
}

#[test]
fn rejects_noncanonical_endpoint_and_duplicate_or_unknown_fields() {
    let fixture = DiscoveryFixture::new();
    let now = OffsetDateTime::now_utc();
    fixture.write_registration(
        now,
        "generation-a",
        "http://localhost:8787",
        CONSUMER_PROTOCOL_VERSION,
    );
    assert!(matches!(
        discover_endpoint(&fixture.root, now),
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
        discover_endpoint(&fixture.root, now),
        Err(InferRuntimeDiscoveryError::InvalidJson(_))
    ));
}

#[cfg(unix)]
#[test]
fn rejects_registration_that_is_not_owner_only() {
    let fixture = DiscoveryFixture::new();
    let now = OffsetDateTime::now_utc();
    fixture.write_registration(
        now,
        "generation-a",
        "http://127.0.0.1:9123",
        CONSUMER_PROTOCOL_VERSION,
    );
    set_mode(&fixture.manifest(), 0o644);
    assert!(matches!(
        discover_endpoint(&fixture.root, now),
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
