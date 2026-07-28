//! Named preset lifecycle and immutable settings-snapshot resolution.

use rusqlite::{OptionalExtension, Transaction, params};
use shadow_domain::EntityId;

use crate::{Catalog, CatalogError, cache_artifact::digest, row_codec::read_id};

use super::{
    model::{
        ExportPresetId, ExportPresetRecord, ExportPresetRevisionId, ExportPresetRevisionRecord,
        ExportSettingsSource,
    },
    row_codec::{read_export_preset, read_export_preset_revision},
    validation::{
        json_digest, normalized_json_object, validate_preset_name, validate_stored_json_snapshot,
    },
};

impl Catalog {
    /// Creates a named preset and its first immutable settings revision.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] for invalid names/settings or a duplicate name.
    pub fn create_export_preset(
        &mut self,
        name: &str,
        settings_json: &str,
        now_ms: i64,
    ) -> Result<ExportPresetRevisionRecord, CatalogError> {
        validate_preset_name(name)?;
        let settings_json = normalized_json_object(settings_json, "preset settings")?;
        let preset_id = ExportPresetId::new_v7();
        let revision_id = ExportPresetRevisionId::new_v7();
        let settings_digest = json_digest(&settings_json);
        let transaction = self.connection.transaction()?;
        transaction.execute(
            "INSERT INTO export_presets(id, name, created_at_ms) VALUES (?1, ?2, ?3)",
            params![preset_id.as_bytes().as_slice(), name.trim(), now_ms],
        )?;
        insert_preset_revision(
            &transaction,
            revision_id,
            preset_id,
            1,
            &settings_json,
            settings_digest,
            now_ms,
        )?;
        transaction.commit()?;
        Ok(ExportPresetRevisionRecord {
            id: revision_id,
            preset_id,
            revision: 1,
            settings_json,
            settings_digest,
            created_at_ms: now_ms,
        })
    }

    /// Appends one immutable revision to a named export preset.
    ///
    /// Existing jobs remain bound to their copied settings snapshot and never
    /// observe this later revision.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the preset is absent, settings are invalid,
    /// revision space is exhausted, or persistence fails.
    pub fn revise_export_preset(
        &mut self,
        preset_id: ExportPresetId,
        settings_json: &str,
        now_ms: i64,
    ) -> Result<ExportPresetRevisionRecord, CatalogError> {
        let settings_json = normalized_json_object(settings_json, "preset settings")?;
        let settings_digest = json_digest(&settings_json);
        let revision_id = ExportPresetRevisionId::new_v7();
        let transaction = self.connection.transaction()?;
        ensure_preset_exists(&transaction, preset_id)?;
        let next_revision = transaction.query_row(
            "SELECT COALESCE(MAX(revision), 0) + 1 FROM export_preset_revisions WHERE preset_id = ?1",
            [preset_id.as_bytes().as_slice()],
            |row| row.get::<_, i64>(0),
        )?;
        let revision = u32::try_from(next_revision).map_err(|_| {
            CatalogError::InvalidExport("export preset revision space is exhausted".into())
        })?;
        insert_preset_revision(
            &transaction,
            revision_id,
            preset_id,
            revision,
            &settings_json,
            settings_digest,
            now_ms,
        )?;
        transaction.commit()?;
        Ok(ExportPresetRevisionRecord {
            id: revision_id,
            preset_id,
            revision,
            settings_json,
            settings_digest,
            created_at_ms: now_ms,
        })
    }

    /// Lists named export presets without materializing their settings history.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when preset rows cannot be read.
    pub fn export_presets(&self) -> Result<Vec<ExportPresetRecord>, CatalogError> {
        let mut statement = self.connection.prepare(
            "SELECT id, name, created_at_ms, archived_at_ms
             FROM export_presets ORDER BY name COLLATE NOCASE, id",
        )?;
        statement
            .query_map([], read_export_preset)?
            .collect::<rusqlite::Result<Vec<_>>>()
            .map_err(Into::into)
    }

    /// Lists every immutable revision of one preset, newest first.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the preset is absent or a stored revision is invalid.
    pub fn export_preset_revisions(
        &self,
        preset_id: ExportPresetId,
    ) -> Result<Vec<ExportPresetRevisionRecord>, CatalogError> {
        ensure_preset_exists_connection(&self.connection, preset_id)?;
        let mut statement = self.connection.prepare(
            "SELECT id, preset_id, revision, settings_json, settings_digest, created_at_ms
             FROM export_preset_revisions WHERE preset_id = ?1
             ORDER BY revision DESC, id DESC",
        )?;
        statement
            .query_map(
                [preset_id.as_bytes().as_slice()],
                read_export_preset_revision,
            )?
            .collect::<rusqlite::Result<Vec<_>>>()
            .map_err(Into::into)
    }
}

fn insert_preset_revision(
    transaction: &Transaction<'_>,
    id: ExportPresetRevisionId,
    preset_id: ExportPresetId,
    revision: u32,
    settings_json: &str,
    settings_digest: [u8; 32],
    created_at_ms: i64,
) -> Result<(), CatalogError> {
    transaction.execute(
        "INSERT INTO export_preset_revisions(
             id, preset_id, revision, settings_json, settings_digest, created_at_ms
         ) VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
        params![
            id.as_bytes().as_slice(),
            preset_id.as_bytes().as_slice(),
            i64::from(revision),
            settings_json,
            settings_digest.as_slice(),
            created_at_ms,
        ],
    )?;
    Ok(())
}

pub(super) fn resolve_settings_snapshot(
    transaction: &Transaction<'_>,
    source: &ExportSettingsSource,
) -> Result<(Option<ExportPresetRevisionId>, String, [u8; 32]), CatalogError> {
    match source {
        ExportSettingsSource::InlineJson(settings_json) => {
            let settings_json = normalized_json_object(settings_json, "export settings")?;
            let digest = json_digest(&settings_json);
            Ok((None, settings_json, digest))
        }
        ExportSettingsSource::PresetRevision(id) => {
            let snapshot = transaction
                .query_row(
                    "SELECT id, settings_json, settings_digest
                     FROM export_preset_revisions WHERE id = ?1",
                    [id.as_bytes().as_slice()],
                    |row| {
                        Ok((
                            read_id::<ExportPresetRevisionId>(row, 0)?,
                            row.get::<_, String>(1)?,
                            digest(row.get(2)?, 2)?,
                        ))
                    },
                )
                .optional()?
                .ok_or(CatalogError::ExportPresetRevisionNotFound(*id))?;
            validate_stored_json_snapshot(&snapshot.1, snapshot.2, "preset settings")?;
            Ok((Some(snapshot.0), snapshot.1, snapshot.2))
        }
    }
}

fn ensure_preset_exists(
    transaction: &Transaction<'_>,
    preset_id: ExportPresetId,
) -> Result<(), CatalogError> {
    let exists = transaction.query_row(
        "SELECT EXISTS(SELECT 1 FROM export_presets WHERE id = ?1)",
        [preset_id.as_bytes().as_slice()],
        |row| row.get::<_, i64>(0),
    )?;
    if exists == 0 {
        return Err(CatalogError::ExportPresetNotFound(preset_id));
    }
    Ok(())
}

fn ensure_preset_exists_connection(
    connection: &rusqlite::Connection,
    preset_id: ExportPresetId,
) -> Result<(), CatalogError> {
    let exists = connection.query_row(
        "SELECT EXISTS(SELECT 1 FROM export_presets WHERE id = ?1)",
        [preset_id.as_bytes().as_slice()],
        |row| row.get::<_, i64>(0),
    )?;
    if exists == 0 {
        return Err(CatalogError::ExportPresetNotFound(preset_id));
    }
    Ok(())
}
