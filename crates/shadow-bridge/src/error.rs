//! Public failure vocabulary shared by every safe bridge contract.

use std::path::PathBuf;

use thiserror::Error;

#[derive(Debug, Error)]
pub enum BridgeError {
    #[error("invalid RAW development plan: {0}")]
    InvalidRawDevelopmentPlan(&'static str),
    #[error("invalid RAW pipeline receipt: {0}")]
    InvalidRawPipelineReceipt(&'static str),
    #[error("invalid edited proxy request: {0}")]
    InvalidEditRequest(&'static str),
    #[error("invalid edit-preview analysis bridge output: {0}")]
    InvalidEditPreviewOutput(&'static str),
    #[error("invalid full edit detail bridge output: {0}")]
    InvalidEditDetailOutput(&'static str),
    #[error("RAW development is unavailable: {0}")]
    RawDevelopmentUnavailable(&'static str),
    #[error("invalid JPEG display-luma request: {0}")]
    InvalidDisplayLumaRequest(&'static str),
    #[error("invalid JPEG display-luma decoder output: {0}")]
    InvalidDisplayLumaOutput(&'static str),
    #[error("the Mac-first decoder bridge currently requires a UTF-8 path: {0}")]
    NonUtf8Path(PathBuf),
    #[error("the C++ decoder bridge returned a null handle")]
    NullHandle,
    #[error("C++ decoder error: {0}")]
    Decoder(#[from] cxx::Exception),
}
