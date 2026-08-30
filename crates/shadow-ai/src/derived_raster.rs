use std::error::Error;

use serde::{Deserialize, Serialize};
use thiserror::Error;

use crate::{
    AiArtifactContractError, AiGeneratedPayload, AiTaskKind, GeneratedArtifactReference,
    LeaseBoundOutput, ModelProvenance, RuntimeContractError,
};

pub const MANAGED_DERIVED_RASTER_RECORD_VERSION: u32 = 1;

/// Application request to copy one rebuildable proposal into managed derived
/// storage without changing its bytes or provenance.
#[derive(Debug)]
pub struct DerivedRasterPromotionRequest {
    promotion_id: String,
    completed: LeaseBoundOutput<AiGeneratedPayload>,
}

impl DerivedRasterPromotionRequest {
    /// Couples a caller-selected promotion identity to runtime-issued output.
    ///
    /// The caller cannot supply or replace provenance because the successful
    /// output envelope is move-only and constructed only by an execution lease.
    pub fn new(promotion_id: String, completed: LeaseBoundOutput<AiGeneratedPayload>) -> Self {
        Self {
            promotion_id,
            completed,
        }
    }

    /// Validates the typed payload and requires a rebuildable source.
    ///
    /// # Errors
    ///
    /// Returns an error when the payload does not match the task, its complete
    /// execution route is invalid, or its generated artifact is malformed.
    fn validate(&self) -> Result<(), DerivedRasterPromotionError> {
        if self.promotion_id.trim().is_empty() {
            return Err(DerivedRasterPromotionError::MissingPromotionId);
        }
        validate_provenance(self.completed.provenance())?;
        self.completed
            .payload()
            .validate_for(self.completed.provenance().task())?;
        Ok(())
    }
}

/// Exact immutable write intent passed only to the application store authority.
///
/// It identifies the proposal bytes the authority must copy, verify, and
/// durably publish before returning a commit.
#[derive(Debug, Copy, Clone)]
pub struct ManagedDerivedStoreWrite<'a> {
    promotion_id: &'a str,
    task: AiTaskKind,
    proposal: &'a GeneratedArtifactReference,
    provenance: &'a ModelProvenance,
}

impl<'a> ManagedDerivedStoreWrite<'a> {
    pub const fn promotion_id(&self) -> &'a str {
        self.promotion_id
    }

    pub const fn task(&self) -> AiTaskKind {
        self.task
    }

    pub const fn proposal(&self) -> &'a GeneratedArtifactReference {
        self.proposal
    }

    pub const fn provenance(&self) -> &'a ModelProvenance {
        self.provenance
    }
}

/// Persisted record presented back to the store for authority verification.
#[derive(Debug, Copy, Clone)]
pub struct ManagedDerivedStoreRead<'a> {
    promotion_id: &'a str,
    task: AiTaskKind,
    payload: &'a AiGeneratedPayload,
    store_object_id: &'a str,
    storage_revision: u32,
    provenance: &'a ModelProvenance,
}

impl<'a> ManagedDerivedStoreRead<'a> {
    pub const fn promotion_id(&self) -> &'a str {
        self.promotion_id
    }

    pub const fn task(&self) -> AiTaskKind {
        self.task
    }

    pub const fn payload(&self) -> &'a AiGeneratedPayload {
        self.payload
    }

    pub const fn store_object_id(&self) -> &'a str {
        self.store_object_id
    }

    pub const fn storage_revision(&self) -> u32 {
        self.storage_revision
    }

    pub const fn provenance(&self) -> &'a ModelProvenance {
        self.provenance
    }
}

/// Result returned by the application-owned store transaction.
///
/// This value is not itself promotion authority: there is no public finalize
/// function that accepts a caller-supplied commit. It is consumed only from a
/// [`ManagedDerivedRasterStore`] call made by [`promote_derived_raster`].
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct ManagedDerivedStoreCommit {
    pub store_object_id: String,
    pub storage_revision: u32,
    pub artifact: GeneratedArtifactReference,
}

/// Durability authority for accepted generated pixels.
///
/// The application implementation—not `shadow-ai`—owns the atomic blob write,
/// verification, durable object identity, rollback, and recovery semantics.
pub trait ManagedDerivedRasterStore {
    type Error: Error + Send + Sync + 'static;

    /// Atomically copies, verifies, and durably publishes the proposed bytes.
    ///
    /// # Errors
    ///
    /// Returns the application store's durability, recovery, or persistence
    /// failure without producing a managed commit.
    fn promote(
        &mut self,
        write: ManagedDerivedStoreWrite<'_>,
    ) -> Result<ManagedDerivedStoreCommit, Self::Error>;

    /// Verifies that a persisted descriptor still names the exact durable
    /// object and bytes before runtime authority is reconstructed.
    ///
    /// # Errors
    ///
    /// Returns the application store's integrity, recovery, or persistence
    /// failure without producing managed authority.
    fn verify(
        &mut self,
        read: ManagedDerivedStoreRead<'_>,
    ) -> Result<ManagedDerivedStoreCommit, Self::Error>;
}

/// Opaque durable authority for one exact generated-byte identity.
///
/// It is intentionally move-only and non-deserializable. Only promotion or a
/// store-verified reload can construct it.
#[derive(Debug, Eq, PartialEq)]
pub struct ManagedGeneratedArtifactReference {
    artifact: GeneratedArtifactReference,
    store_object_id: String,
    storage_revision: u32,
}

impl ManagedGeneratedArtifactReference {
    pub const fn artifact(&self) -> &GeneratedArtifactReference {
        &self.artifact
    }

    pub fn store_object_id(&self) -> &str {
        &self.store_object_id
    }

    pub const fn storage_revision(&self) -> u32 {
        self.storage_revision
    }
}

/// One managed payload established by the application store authority.
///
/// This authority object is move-only and non-deserializable.
#[derive(Debug)]
pub struct ManagedDerivedRaster {
    promotion_id: String,
    task: AiTaskKind,
    payload: AiGeneratedPayload,
    managed_artifact: ManagedGeneratedArtifactReference,
    provenance: ModelProvenance,
}

impl ManagedDerivedRaster {
    pub fn promotion_id(&self) -> &str {
        &self.promotion_id
    }

    pub const fn task(&self) -> AiTaskKind {
        self.task
    }

    pub const fn payload(&self) -> &AiGeneratedPayload {
        &self.payload
    }

    pub const fn managed_artifact(&self) -> &ManagedGeneratedArtifactReference {
        &self.managed_artifact
    }

    pub const fn provenance(&self) -> &ModelProvenance {
        &self.provenance
    }

    /// Produces a serializable descriptor, not reload authority.
    pub fn persisted_record(&self) -> PersistedManagedDerivedRaster {
        PersistedManagedDerivedRaster {
            contract_version: MANAGED_DERIVED_RASTER_RECORD_VERSION,
            promotion_id: self.promotion_id.clone(),
            task: self.task,
            payload: self.payload.clone(),
            store_object_id: self.managed_artifact.store_object_id.clone(),
            storage_revision: self.managed_artifact.storage_revision,
            provenance: self.provenance.clone(),
        }
    }
}

/// Serializable durable descriptor. Deserializing it does not reconstruct
/// managed authority; [`reload_managed_derived_raster`] must verify it through
/// the application store.
#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct PersistedManagedDerivedRaster {
    contract_version: u32,
    promotion_id: String,
    task: AiTaskKind,
    payload: AiGeneratedPayload,
    store_object_id: String,
    storage_revision: u32,
    provenance: ModelProvenance,
}

/// Runs proposal validation and the application store transaction, then
/// returns one internally consistent managed payload.
///
/// # Errors
///
/// Returns a contract error if the proposal or store commit is inconsistent,
/// or the store authority's own error if durability could not be established.
pub fn promote_derived_raster<S>(
    store: &mut S,
    request: DerivedRasterPromotionRequest,
) -> Result<ManagedDerivedRaster, DerivedRasterPromotionFailure<S::Error>>
where
    S: ManagedDerivedRasterStore,
{
    request
        .validate()
        .map_err(DerivedRasterPromotionFailure::Contract)?;
    let promotion_id = request.promotion_id;
    let (mut payload, provenance) = request.completed.into_parts();
    let task = provenance.task();
    let proposal = payload_artifact(&payload).clone();
    let commit = store
        .promote(ManagedDerivedStoreWrite {
            promotion_id: &promotion_id,
            task,
            proposal: &proposal,
            provenance: &provenance,
        })
        .map_err(DerivedRasterPromotionFailure::Store)?;
    finish_managed(promotion_id, task, &mut payload, provenance, commit, None)
        .map_err(DerivedRasterPromotionFailure::Contract)
}

/// Reconstructs managed authority only after the application store verifies a
/// deserialized descriptor.
///
/// # Errors
///
/// Returns a contract error for a malformed or substituted record, or the
/// store's own verification error.
pub fn reload_managed_derived_raster<S>(
    store: &mut S,
    record: PersistedManagedDerivedRaster,
) -> Result<ManagedDerivedRaster, DerivedRasterPromotionFailure<S::Error>>
where
    S: ManagedDerivedRasterStore,
{
    validate_persisted_record(&record).map_err(DerivedRasterPromotionFailure::Contract)?;
    let commit = store
        .verify(ManagedDerivedStoreRead {
            promotion_id: &record.promotion_id,
            task: record.task,
            payload: &record.payload,
            store_object_id: &record.store_object_id,
            storage_revision: record.storage_revision,
            provenance: &record.provenance,
        })
        .map_err(DerivedRasterPromotionFailure::Store)?;
    let expected_store = (record.store_object_id.as_str(), record.storage_revision);
    let mut payload = record.payload;
    finish_managed(
        record.promotion_id,
        record.task,
        &mut payload,
        record.provenance,
        commit,
        Some(expected_store),
    )
    .map_err(DerivedRasterPromotionFailure::Contract)
}

fn finish_managed(
    promotion_id: String,
    task: AiTaskKind,
    payload: &mut AiGeneratedPayload,
    provenance: ModelProvenance,
    commit: ManagedDerivedStoreCommit,
    expected_store: Option<(&str, u32)>,
) -> Result<ManagedDerivedRaster, DerivedRasterPromotionError> {
    let proposal = payload_artifact(payload).clone();
    validate_store_commit(&proposal, &commit)?;
    if let Some((store_object_id, storage_revision)) = expected_store
        && (commit.store_object_id != store_object_id
            || commit.storage_revision != storage_revision)
    {
        return Err(DerivedRasterPromotionError::StoreRecordMismatch);
    }
    *payload_artifact_mut(payload) = commit.artifact.clone();
    payload.validate_for(task)?;
    Ok(ManagedDerivedRaster {
        promotion_id,
        task,
        payload: payload.clone(),
        managed_artifact: ManagedGeneratedArtifactReference {
            artifact: commit.artifact,
            store_object_id: commit.store_object_id,
            storage_revision: commit.storage_revision,
        },
        provenance,
    })
}

fn validate_persisted_record(
    record: &PersistedManagedDerivedRaster,
) -> Result<(), DerivedRasterPromotionError> {
    if record.contract_version != MANAGED_DERIVED_RASTER_RECORD_VERSION {
        return Err(DerivedRasterPromotionError::UnsupportedPersistedRecord(
            record.contract_version,
        ));
    }
    if record.promotion_id.trim().is_empty() {
        return Err(DerivedRasterPromotionError::MissingPromotionId);
    }
    if record.store_object_id.trim().is_empty() {
        return Err(DerivedRasterPromotionError::MissingStoreObjectId);
    }
    if record.storage_revision == 0 {
        return Err(DerivedRasterPromotionError::InvalidStorageRevision);
    }
    validate_provenance(&record.provenance)?;
    record.payload.validate_for(record.task)?;
    Ok(())
}

fn validate_store_commit(
    proposal: &GeneratedArtifactReference,
    commit: &ManagedDerivedStoreCommit,
) -> Result<(), DerivedRasterPromotionError> {
    commit.artifact.validate()?;
    if commit.store_object_id.trim().is_empty() {
        return Err(DerivedRasterPromotionError::MissingStoreObjectId);
    }
    if commit.storage_revision == 0 {
        return Err(DerivedRasterPromotionError::InvalidStorageRevision);
    }
    if !same_artifact_bytes(proposal, &commit.artifact) {
        return Err(DerivedRasterPromotionError::ArtifactIdentityChanged);
    }
    Ok(())
}

fn payload_artifact(payload: &AiGeneratedPayload) -> &GeneratedArtifactReference {
    match payload {
        AiGeneratedPayload::SoftMask(mask) => &mask.artifact,
        AiGeneratedPayload::DenoisedRaster(raster) => &raster.artifact,
        AiGeneratedPayload::ImageCompletionPatch(patch) => &patch.artifact,
    }
}

fn payload_artifact_mut(payload: &mut AiGeneratedPayload) -> &mut GeneratedArtifactReference {
    match payload {
        AiGeneratedPayload::SoftMask(mask) => &mut mask.artifact,
        AiGeneratedPayload::DenoisedRaster(raster) => &mut raster.artifact,
        AiGeneratedPayload::ImageCompletionPatch(patch) => &mut patch.artifact,
    }
}

fn same_artifact_bytes(
    proposal: &GeneratedArtifactReference,
    managed: &GeneratedArtifactReference,
) -> bool {
    proposal == managed
}

fn validate_provenance(provenance: &ModelProvenance) -> Result<(), DerivedRasterPromotionError> {
    provenance.validate()?;
    Ok(())
}

#[derive(Debug, Clone, PartialEq, Error)]
pub enum DerivedRasterPromotionError {
    #[error("promotion_id must not be empty")]
    MissingPromotionId,
    #[error("managed derived-store commit must name its durable object")]
    MissingStoreObjectId,
    #[error("managed derived-store commit must name a non-zero storage revision")]
    InvalidStorageRevision,
    #[error("managed derived-store commit changed the generated artifact identity")]
    ArtifactIdentityChanged,
    #[error("persisted managed-raster record version {0} is unsupported")]
    UnsupportedPersistedRecord(u32),
    #[error("managed derived-store verification disagrees with the persisted store identity")]
    StoreRecordMismatch,
    #[error(transparent)]
    InvalidExecutionRoute(#[from] RuntimeContractError),
    #[error(transparent)]
    InvalidArtifact(#[from] AiArtifactContractError),
}

#[derive(Debug, Error)]
pub enum DerivedRasterPromotionFailure<E>
where
    E: Error + Send + Sync + 'static,
{
    #[error(transparent)]
    Contract(DerivedRasterPromotionError),
    #[error("managed derived-store transaction failed")]
    Store(#[source] E),
}

#[cfg(test)]
mod tests;
