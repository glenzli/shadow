//! Auditable remote-provider facts and fail-closed request admission.
//!
//! [`manifest`] owns stable service/model/legal declarations. [`upload`] wraps
//! store-verified sanitized outbound bytes. [`admission`] binds one exact
//! [`crate::AiJobRequest`] plus those opaque receipts to that manifest.
//! Transport, credentials, billing, and actual network execution remain
//! application responsibilities.

mod admission;
mod manifest;
mod upload;

pub use admission::{
    MAX_REMOTE_INPUTS, REMOTE_EXECUTION_REQUEST_SCHEMA_VERSION, RemoteExecutionContext,
    RemoteExecutionDenied, RemoteExecutionGrant, RemoteExecutionPolicy, RemoteProviderBlocker,
    RemoteTransportExecution, admit_remote_execution,
};
pub use manifest::{
    REMOTE_PROVIDER_MANIFEST_SCHEMA_VERSION, RemoteDataRetention, RemoteOfflineBehavior,
    RemoteProviderManifest, RemoteProviderManifestError, RemoteTrainingUse, RemoteUploadScope,
};
pub use upload::{
    PREPARED_REMOTE_UPLOAD_CONTRACT_VERSION, PreparedRemoteUpload, PreparedRemoteUploadCommit,
    PreparedRemoteUploadError, RemoteUploadPreparation, RemoteUploadPreparationFailure,
    RemoteUploadStore, prepare_remote_upload,
};

#[cfg(test)]
mod tests;
