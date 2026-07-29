use std::time::{SystemTime, UNIX_EPOCH};

use crate::{AiJobRequest, ExecutionRouteIdentity};

use super::{
    RemoteProviderBlocker, identity::RemoteGrantIdentity, validation::ValidatedRemoteUploads,
};
use crate::remote::PreparedRemoteUpload;

const REMOTE_GRANT_LIFETIME_MS: u64 = 60_000;

/// Move-only remote execution permission bound to one complete request,
/// manifest, consent decision, and exact sanitized outbound inventory.
///
/// It is intentionally non-cloneable and non-deserializable. Consuming it
/// checks freshness and produces a single transport envelope carrying a stable
/// idempotency key.
#[derive(Debug)]
pub struct RemoteExecutionGrant {
    request: AiJobRequest,
    request_blake3: String,
    route: ExecutionRouteIdentity,
    manifest_blake3: String,
    legal_policy_blake3: String,
    consent_policy_blake3: String,
    uploads: Vec<PreparedRemoteUpload>,
    total_upload_bytes: u64,
    issued_at_unix_ms: u64,
    expires_at_unix_ms: u64,
    idempotency_key: String,
}

impl RemoteExecutionGrant {
    pub(super) fn issue(
        request: AiJobRequest,
        uploads: ValidatedRemoteUploads,
        identity: RemoteGrantIdentity,
    ) -> Result<Self, RemoteProviderBlocker> {
        let issued_at_unix_ms = unix_time_ms().ok_or(RemoteProviderBlocker::ClockUnavailable)?;
        let expires_at_unix_ms = issued_at_unix_ms
            .checked_add(REMOTE_GRANT_LIFETIME_MS)
            .ok_or(RemoteProviderBlocker::ClockUnavailable)?;
        let (uploads, total_upload_bytes) = uploads.into_parts();
        Ok(Self {
            request,
            request_blake3: identity.request_blake3,
            route: identity.route,
            manifest_blake3: identity.manifest_blake3,
            legal_policy_blake3: identity.legal_policy_blake3,
            consent_policy_blake3: identity.consent_policy_blake3,
            uploads,
            total_upload_bytes,
            issued_at_unix_ms,
            expires_at_unix_ms,
            idempotency_key: identity.idempotency_key,
        })
    }

    pub fn request_id(&self) -> &str {
        &self.request.request_id
    }

    pub const fn generation(&self) -> u64 {
        self.request.generation
    }

    pub fn request_blake3(&self) -> &str {
        &self.request_blake3
    }

    pub const fn route(&self) -> &ExecutionRouteIdentity {
        &self.route
    }

    pub fn manifest_blake3(&self) -> &str {
        &self.manifest_blake3
    }

    pub fn legal_policy_blake3(&self) -> &str {
        &self.legal_policy_blake3
    }

    pub fn consent_policy_blake3(&self) -> &str {
        &self.consent_policy_blake3
    }

    pub const fn total_upload_bytes(&self) -> u64 {
        self.total_upload_bytes
    }

    pub const fn issued_at_unix_ms(&self) -> u64 {
        self.issued_at_unix_ms
    }

    pub const fn expires_at_unix_ms(&self) -> u64 {
        self.expires_at_unix_ms
    }

    pub fn idempotency_key(&self) -> &str {
        &self.idempotency_key
    }

    /// Consumes the permission and checks its short runtime-owned expiry.
    ///
    /// # Errors
    ///
    /// Returns `GrantExpired` when the grant is no longer fresh or the system
    /// clock cannot supply a portable Unix timestamp.
    pub fn into_transport_execution(
        self,
    ) -> Result<RemoteTransportExecution, RemoteProviderBlocker> {
        let now = unix_time_ms().ok_or(RemoteProviderBlocker::ClockUnavailable)?;
        self.into_transport_execution_at(now)
    }

    fn into_transport_execution_at(
        self,
        now_unix_ms: u64,
    ) -> Result<RemoteTransportExecution, RemoteProviderBlocker> {
        if now_unix_ms > self.expires_at_unix_ms {
            return Err(RemoteProviderBlocker::GrantExpired);
        }
        Ok(RemoteTransportExecution {
            request: self.request,
            request_blake3: self.request_blake3,
            route: self.route,
            manifest_blake3: self.manifest_blake3,
            legal_policy_blake3: self.legal_policy_blake3,
            consent_policy_blake3: self.consent_policy_blake3,
            uploads: self.uploads,
            total_upload_bytes: self.total_upload_bytes,
            issued_at_unix_ms: self.issued_at_unix_ms,
            expires_at_unix_ms: self.expires_at_unix_ms,
            idempotency_key: self.idempotency_key,
        })
    }

    #[cfg(test)]
    pub(crate) fn into_transport_execution_at_for_test(
        self,
        now_unix_ms: u64,
    ) -> Result<RemoteTransportExecution, RemoteProviderBlocker> {
        self.into_transport_execution_at(now_unix_ms)
    }
}

/// Single-use transport envelope produced by consuming a fresh grant.
///
/// This type is also move-only and non-deserializable. A transport adapter may
/// borrow the exact request and outbound-store receipts, but cannot substitute
/// another request or reconstitute authority from persisted JSON.
#[derive(Debug)]
pub struct RemoteTransportExecution {
    request: AiJobRequest,
    request_blake3: String,
    route: ExecutionRouteIdentity,
    manifest_blake3: String,
    legal_policy_blake3: String,
    consent_policy_blake3: String,
    uploads: Vec<PreparedRemoteUpload>,
    total_upload_bytes: u64,
    issued_at_unix_ms: u64,
    expires_at_unix_ms: u64,
    idempotency_key: String,
}

impl RemoteTransportExecution {
    pub const fn request(&self) -> &AiJobRequest {
        &self.request
    }

    pub fn request_blake3(&self) -> &str {
        &self.request_blake3
    }

    pub const fn route(&self) -> &ExecutionRouteIdentity {
        &self.route
    }

    pub fn manifest_blake3(&self) -> &str {
        &self.manifest_blake3
    }

    pub fn legal_policy_blake3(&self) -> &str {
        &self.legal_policy_blake3
    }

    pub fn consent_policy_blake3(&self) -> &str {
        &self.consent_policy_blake3
    }

    pub fn uploads(&self) -> &[PreparedRemoteUpload] {
        &self.uploads
    }

    pub const fn total_upload_bytes(&self) -> u64 {
        self.total_upload_bytes
    }

    pub const fn issued_at_unix_ms(&self) -> u64 {
        self.issued_at_unix_ms
    }

    pub const fn expires_at_unix_ms(&self) -> u64 {
        self.expires_at_unix_ms
    }

    pub fn idempotency_key(&self) -> &str {
        &self.idempotency_key
    }
}

fn unix_time_ms() -> Option<u64> {
    let duration = SystemTime::now().duration_since(UNIX_EPOCH).ok()?;
    u64::try_from(duration.as_millis()).ok()
}
