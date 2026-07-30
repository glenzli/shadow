//! Library metadata workflows that are independent of the desktop UI.
//!
//! [`gpx`] owns bounded GPX parsing and deterministic timestamp matching. The
//! Catalog remains the owner of persisted user overrides.

mod gpx;

pub use gpx::{
    GpsMatchPreview, GpsMatchProposal, GpsMatchSettings, GpsPhotoCapture, GpxImportError, GpxTrack,
    GpxTrackPoint, load_gpx_track, match_photos_to_gpx,
};

#[cfg(test)]
mod tests;
