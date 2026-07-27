//! Client adapters for named export presets and immutable revisions.

use crate::{CatalogError, ExportPresetId, ExportPresetRecord, ExportPresetRevisionRecord};

use super::super::{
    CatalogHandle,
    protocol::{ExportPresetMessage, Message},
};

impl CatalogHandle {
    /// Creates a named export preset and its first immutable settings revision
    /// through the single catalog writer.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the actor is unavailable or the preset is invalid.
    pub fn create_export_preset(
        &self,
        name: &str,
        settings_json: &str,
        now_ms: i64,
    ) -> Result<ExportPresetRevisionRecord, CatalogError> {
        self.request(|response| {
            Message::ExportPreset(ExportPresetMessage::Create(
                name.to_owned(),
                settings_json.to_owned(),
                now_ms,
                response,
            ))
        })
    }

    /// Appends one immutable revision to an existing export preset.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the actor is unavailable or the revision is invalid.
    pub fn revise_export_preset(
        &self,
        preset_id: ExportPresetId,
        settings_json: &str,
        now_ms: i64,
    ) -> Result<ExportPresetRevisionRecord, CatalogError> {
        self.request(|response| {
            Message::ExportPreset(ExportPresetMessage::Revise(
                preset_id,
                settings_json.to_owned(),
                now_ms,
                response,
            ))
        })
    }

    /// Lists named export presets through the catalog actor.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the actor is unavailable or the query fails.
    pub fn export_presets(&self) -> Result<Vec<ExportPresetRecord>, CatalogError> {
        self.request(|response| Message::ExportPreset(ExportPresetMessage::List(response)))
    }

    /// Lists all immutable revisions for one export preset.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the actor is unavailable or the preset is absent.
    pub fn export_preset_revisions(
        &self,
        preset_id: ExportPresetId,
    ) -> Result<Vec<ExportPresetRevisionRecord>, CatalogError> {
        self.request(|response| {
            Message::ExportPreset(ExportPresetMessage::Revisions(preset_id, response))
        })
    }
}
