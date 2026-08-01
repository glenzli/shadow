//! Provider-neutral reverse-geocoding candidate and result projection.
//!
//! Online/native providers belong to the desktop shell. This service only
//! exposes bounded Catalog work and converts a successful provider response
//! into the exact coordinate-bound persistence contract.

use anyhow::Result as AnyResult;
use shadow_catalog::{RecordLibraryPlaceResolution, RecordLibraryPlaceResolutionStatus};

use crate::ffi;

use super::LibraryService;

impl LibraryService {
    pub(crate) fn place_resolution_candidates(
        &self,
        limit: u32,
    ) -> AnyResult<Vec<ffi::FfiLibraryPlaceResolutionCandidate>> {
        Ok(self
            .catalog
            .library_place_resolution_candidates(usize::try_from(limit).unwrap_or(usize::MAX))?
            .into_iter()
            .map(|candidate| ffi::FfiLibraryPlaceResolutionCandidate {
                latitude_e7: candidate.latitude_e7,
                longitude_e7: candidate.longitude_e7,
                photo_count: candidate.photo_count,
            })
            .collect())
    }

    pub(crate) fn record_place_resolution(
        &self,
        result: &ffi::FfiLibraryPlaceResolutionResult,
        resolved_at_ms: i64,
    ) -> AnyResult<ffi::FfiRecordLibraryPlaceResolutionStatus> {
        let status =
            self.catalog
                .record_library_place_resolution(&RecordLibraryPlaceResolution {
                    latitude_e7: result.latitude_e7,
                    longitude_e7: result.longitude_e7,
                    country_code: result.country_code.clone(),
                    country_name: result.country_name.clone(),
                    administrative_area: result.administrative_area.clone(),
                    locality: result.locality.clone(),
                    display_name: result.display_name.clone(),
                    provider_id: result.provider_id.clone(),
                    provider_version: result.provider_version.clone(),
                    locale: result.locale.clone(),
                    resolved_at_ms,
                })?;
        Ok(match status {
            RecordLibraryPlaceResolutionStatus::Recorded => {
                ffi::FfiRecordLibraryPlaceResolutionStatus::Recorded
            }
            RecordLibraryPlaceResolutionStatus::CoordinatesNoLongerUsed => {
                ffi::FfiRecordLibraryPlaceResolutionStatus::CoordinatesNoLongerUsed
            }
        })
    }
}

#[cfg(test)]
mod tests;
