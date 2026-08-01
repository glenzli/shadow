//! Actor messages for coordinate-bound reverse-geocoding persistence.

use std::sync::mpsc::SyncSender;

use crate::{
    CatalogError, LibraryPlaceResolution, LibraryPlaceResolutionCandidate,
    RecordLibraryPlaceResolution, RecordLibraryPlaceResolutionStatus,
};

pub(in crate::writer) enum LibraryPlaceResolutionMessage {
    Candidates(
        usize,
        SyncSender<Result<Vec<LibraryPlaceResolutionCandidate>, CatalogError>>,
    ),
    Record(
        RecordLibraryPlaceResolution,
        SyncSender<Result<RecordLibraryPlaceResolutionStatus, CatalogError>>,
    ),
    Read(
        i32,
        i32,
        SyncSender<Result<Option<LibraryPlaceResolution>, CatalogError>>,
    ),
}
