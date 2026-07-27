//! Actor-side execution for export-preset messages.

use crate::Catalog;

use super::super::protocol::ExportPresetMessage;

pub(super) fn run_export_preset_message(catalog: &mut Catalog, message: ExportPresetMessage) {
    match message {
        ExportPresetMessage::Create(name, settings_json, now_ms, response) => {
            let _ = response.send(catalog.create_export_preset(&name, &settings_json, now_ms));
        }
        ExportPresetMessage::Revise(preset_id, settings_json, now_ms, response) => {
            let _ = response.send(catalog.revise_export_preset(preset_id, &settings_json, now_ms));
        }
        ExportPresetMessage::List(response) => {
            let _ = response.send(catalog.export_presets());
        }
        ExportPresetMessage::Revisions(preset_id, response) => {
            let _ = response.send(catalog.export_preset_revisions(preset_id));
        }
    }
}
