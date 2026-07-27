//! Responsibility-indexed unit tests for the desktop bridge facade.
//!
//! Keep private facade invariants in this crate-local tree. Put tests beside a
//! production service when that service owns the behavior, and reserve this
//! index for contracts that genuinely cross desktop bridge responsibilities.

use std::{collections::BTreeSet, sync::Arc, thread};

use rusqlite::{Connection, params};
use shadow_ai::{
    FeedbackAction, FeedbackIgnored, IncrementalTrainingPolicy, LearningScope, NewFeedbackEvent,
    PairwiseOutcome, PresentationContext, PresentedFitMode, UnitInterval as AiUnitInterval,
    build_incremental_preference_batch,
};
use shadow_cache::ContentAddressedStore;
use shadow_catalog::{
    CachedArtifact, CachedArtifactRecord, LibraryPhotoFacts, RecordCachedArtifact,
    RecordDecodeSnapshot, RegisterAsset, RepresentationFingerprint,
};
use shadow_core::DecodeInspector;
use shadow_domain::{
    AssetLocation, DecodeCapabilitySnapshot, DecodeProviderSnapshot, DecodeSupport,
    DecoderSnapshot, EntityId, ImageDimensions, ImageMargins, ImportSessionId, MAX_PHOTO_RATING,
    PendingCorrectionsSnapshot, PhotoDecisionOrigin, Platform, PreviewByteOrder, PreviewCodec,
    RawMetadataSnapshot, RepresentationId, RepresentationKind,
};

use crate::isolated_proxy::NativeDecodeAdmission;
use crate::photo_provider::{PHOTO_GRID_PROXY_JPEG_QUALITY, PHOTO_GRID_PROXY_MAX_EDGE};
use crate::review_service::{
    REVIEW_COMPARE_DECODER_ID, REVIEW_COMPARE_PIXEL_FORMAT, REVIEW_COMPARE_PIXEL_HASH_ALGORITHM,
    REVIEW_COMPARE_SURFACE_ID, REVIEW_COMPARE_SURFACE_REVISION, ReviewVisualSelection, file_name,
    parse_cursor,
};
use crate::session_edit_history::{
    LIBRARY_EDIT_MAIN_REF, NAMED_VERSION_REF_PREFIX, WORKING_RECIPE_REF,
};
#[cfg(any())]
use crate::session_edit_history::{LIBRARY_EDIT_VERSION_REF_PREFIX, LIBRARY_PHOTO_EDIT_KEY_PREFIX};
use crate::session_edit_render::{
    EditPreviewPolicy, admits_recipe_preview_cache, requested_raw_development_plan_cache_matches,
};
use crate::session_photo_source::{
    MissingCatalogOpticsRoute, missing_catalog_optics_route, query_missing_catalog_optics_profiles,
    reject_quarantined_native_decode,
};

use super::*;

mod adjustment_contract;
mod edit_sessions;
mod facade;
mod library;
mod preview_and_detail;
#[cfg(any())]
mod raw_fixtures;
mod raw_inspection;
mod recipe_compiler;
mod recipe_identity;
mod review;
mod support;

use support::*;
