use rusqlite::{OptionalExtension, Transaction, params, types::Type};
use shadow_domain::{DecoderSnapshot, EntityId, RepresentationId};

use crate::{
    Catalog, CatalogError, TechnicalObservationRevision,
    library_metadata::project_decoder_metadata_into_library_facts, read_id,
};

const SNAPSHOT_SCHEMA: i64 = 1;

/// The source identity observed when a decode inspection was scheduled.
///
/// Catalog snapshots are caches. Matching this fingerprint prevents a slow
/// decoder result for an older file revision from replacing current state.
#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub struct RepresentationFingerprint {
    pub byte_len: u64,
    pub modified_at_ms: Option<i64>,
}

#[derive(Debug, Clone, PartialEq)]
pub struct RecordDecodeSnapshot {
    pub representation_id: RepresentationId,
    pub expected_source: RepresentationFingerprint,
    pub snapshot: DecoderSnapshot,
    pub inspected_at_ms: i64,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub enum RecordDecodeSnapshotStatus {
    Recorded,
    StaleSource,
}

#[derive(Debug, Clone, PartialEq)]
pub struct DecodeSnapshotRecord {
    pub representation_id: RepresentationId,
    pub source: RepresentationFingerprint,
    pub inspected_at_ms: i64,
    pub snapshot: DecoderSnapshot,
}

impl Catalog {
    /// Returns the source fingerprint currently owned by a representation.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError::RepresentationNotFound`] when the id is absent,
    /// or a catalog error when the stored size is invalid.
    pub fn representation_fingerprint(
        &self,
        representation_id: RepresentationId,
    ) -> Result<RepresentationFingerprint, CatalogError> {
        representation_fingerprint(&self.connection, representation_id)
    }

    /// Transactionally records one provider-neutral decoder snapshot.
    ///
    /// A provider id is a replaceable cache slot: a newer `LibRaw` snapshot
    /// replaces an older `LibRaw` snapshot and its preview descriptors, while a
    /// Nikon or DNG provider may coexist for the same representation.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the representation is absent, the snapshot
    /// is invalid, serialization fails, or `SQLite` cannot commit the update.
    pub fn record_decode_snapshot(
        &mut self,
        request: &RecordDecodeSnapshot,
    ) -> Result<RecordDecodeSnapshotStatus, CatalogError> {
        validate_snapshot(&request.snapshot)?;
        let snapshot_json = serde_json::to_string(&request.snapshot)?;
        let transaction = self.connection.transaction()?;
        let current =
            representation_fingerprint_in_transaction(&transaction, request.representation_id)?;

        if current != request.expected_source {
            return Ok(RecordDecodeSnapshotStatus::StaleSource);
        }

        upsert_snapshot(&transaction, request, &snapshot_json)?;
        replace_previews(&transaction, request)?;
        if request.snapshot.capabilities.metadata.is_available() {
            project_decoder_metadata_into_library_facts(
                &transaction,
                request.representation_id,
                request.expected_source,
                &request.snapshot.metadata,
                request.inspected_at_ms,
            )?;
        }
        transaction.commit()?;
        Ok(RecordDecodeSnapshotStatus::Recorded)
    }

    /// Returns all current decoder-provider snapshots for a representation.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the representation is absent, persisted JSON
    /// is invalid, or the query fails.
    pub fn decode_snapshots(
        &self,
        representation_id: RepresentationId,
    ) -> Result<Vec<DecodeSnapshotRecord>, CatalogError> {
        self.representation_fingerprint(representation_id)?;
        let mut statement = self.connection.prepare(
            "SELECT representation_id, provider_id, provider_version, snapshot_schema,
                    snapshot_json, source_byte_len, source_modified_at_ms, inspected_at_ms
             FROM representation_decode_snapshots
             WHERE representation_id = ?1
             ORDER BY provider_id",
        )?;
        let rows = statement.query_map([representation_id.as_bytes().as_slice()], |row| {
            let byte_len: i64 = row.get(5)?;
            Ok((
                read_id(row, 0)?,
                row.get::<_, String>(1)?,
                row.get::<_, String>(2)?,
                row.get::<_, i64>(3)?,
                row.get::<_, String>(4)?,
                non_negative_u64(byte_len, 5)?,
                row.get::<_, Option<i64>>(6)?,
                row.get::<_, i64>(7)?,
            ))
        })?;

        let mut snapshots = Vec::new();
        for row in rows {
            let (
                stored_representation_id,
                provider_id,
                provider_version,
                schema,
                json,
                byte_len,
                modified_at_ms,
                inspected_at_ms,
            ) = row?;
            if schema != SNAPSHOT_SCHEMA {
                return Err(CatalogError::UnsupportedDecodeSnapshotSchema(schema));
            }
            let snapshot: DecoderSnapshot = serde_json::from_str(&json)?;
            if snapshot.provider.id != provider_id || snapshot.provider.version != provider_version
            {
                return Err(CatalogError::InvalidDecodeSnapshot(
                    "provider columns disagree with snapshot JSON",
                ));
            }
            snapshots.push(DecodeSnapshotRecord {
                representation_id: stored_representation_id,
                source: RepresentationFingerprint {
                    byte_len,
                    modified_at_ms,
                },
                inspected_at_ms,
                snapshot,
            });
        }
        Ok(snapshots)
    }

    /// Reports whether a provider's required decode output is current for this
    /// exact source revision.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the representation is absent or the query
    /// fails.
    #[allow(clippy::too_many_arguments)]
    pub fn is_decode_output_current(
        &self,
        representation_id: RepresentationId,
        provider_id: &str,
        provider_version: &str,
        source: RepresentationFingerprint,
        require_cached_preview: bool,
        proxy_variant_key: &str,
        required_technical_preprocessing: Option<&str>,
    ) -> Result<bool, CatalogError> {
        if self.representation_fingerprint(representation_id)? != source {
            return Ok(false);
        }
        let byte_len = sqlite_u64(source.byte_len, "source_byte_len")?;
        let current: Option<(i64, i64, i64, i64)> = self
            .connection
            .query_row(
                "SELECT s.has_embedded_previews,
                        s.can_render_reference_rgb,
                        EXISTS(
                            SELECT 1 FROM representation_cached_artifacts a
                            WHERE a.representation_id = s.representation_id
                              AND a.role = 'embedded_preview'
                              AND a.variant_key = s.provider_id
                              AND a.generator_id = s.provider_id
                              AND a.generator_version = s.provider_version
                              AND a.source_byte_len = s.source_byte_len
                              AND a.source_modified_at_ms IS s.source_modified_at_ms
                        ),
                        EXISTS(
                            SELECT 1 FROM representation_cached_artifacts a
                            WHERE a.representation_id = s.representation_id
                              AND a.role = 'generated_proxy'
                              AND a.variant_key = ?6
                              AND a.generator_id = s.provider_id
                              AND a.generator_version = s.provider_version
                              AND a.source_byte_len = s.source_byte_len
                              AND a.source_modified_at_ms IS s.source_modified_at_ms
                        )
                 FROM representation_decode_snapshots s
                 WHERE s.representation_id = ?1 AND s.provider_id = ?2
                   AND s.provider_version = ?3
                   AND s.source_byte_len = ?4
                   AND s.source_modified_at_ms IS ?5",
                params![
                    representation_id.as_bytes().as_slice(),
                    provider_id,
                    provider_version,
                    byte_len,
                    source.modified_at_ms,
                    proxy_variant_key,
                ],
                |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?, row.get(3)?)),
            )
            .optional()?;
        let output_current =
            current.is_some_and(|(has_preview, can_render, cached_preview, cached_proxy)| {
                !require_cached_preview
                    || if has_preview != 0 {
                        cached_preview != 0
                    } else if can_render != 0 {
                        cached_proxy != 0
                    } else {
                        true
                    }
            });
        if !output_current {
            return Ok(false);
        }
        let Some(preprocessing_version) = required_technical_preprocessing else {
            return Ok(true);
        };
        preferred_visual_has_current_technical_observation(
            self,
            representation_id,
            source,
            &TechnicalObservationRevision::current(preprocessing_version),
        )
    }
}

fn preferred_visual_has_current_technical_observation(
    catalog: &Catalog,
    representation_id: RepresentationId,
    source: RepresentationFingerprint,
    revision: &TechnicalObservationRevision,
) -> Result<bool, CatalogError> {
    if !revision.is_supported_by_this_build() {
        return Ok(false);
    }
    let preferred = catalog.preferred_cached_artifact(representation_id)?;
    Ok(match preferred {
        // No supported visual means there is nothing for this observer to do.
        None => true,
        Some(record) if record.artifact.codec != shadow_domain::PreviewCodec::Jpeg => true,
        Some(record) if record.source != source => false,
        Some(record) => match catalog.technical_observation(
            representation_id,
            source,
            &record.artifact,
            revision,
        ) {
            Ok(value) => value.is_some(),
            Err(
                CatalogError::InvalidPersistedTechnicalObservation(_)
                | CatalogError::TechnicalObservationJson(_),
            ) => false,
            Err(error) => return Err(error),
        },
    })
}

fn validate_snapshot(snapshot: &DecoderSnapshot) -> Result<(), CatalogError> {
    if snapshot.provider.id.is_empty() {
        return Err(CatalogError::InvalidDecodeSnapshot(
            "provider id must not be empty",
        ));
    }
    if !snapshot.metadata.baseline_exposure.is_finite()
        || snapshot
            .metadata
            .as_shot_neutral
            .iter()
            .any(|component| !component.is_finite())
    {
        return Err(CatalogError::InvalidDecodeSnapshot(
            "floating-point metadata must be finite",
        ));
    }
    Ok(())
}

fn representation_fingerprint(
    connection: &rusqlite::Connection,
    representation_id: RepresentationId,
) -> Result<RepresentationFingerprint, CatalogError> {
    let value = connection
        .query_row(
            "SELECT byte_len, modified_at_ms FROM representations WHERE id = ?1",
            [representation_id.as_bytes().as_slice()],
            |row| {
                let byte_len: i64 = row.get(0)?;
                Ok(RepresentationFingerprint {
                    byte_len: non_negative_u64(byte_len, 0)?,
                    modified_at_ms: row.get(1)?,
                })
            },
        )
        .optional()?;
    value.ok_or(CatalogError::RepresentationNotFound(representation_id))
}

pub(crate) fn representation_fingerprint_in_transaction(
    transaction: &Transaction<'_>,
    representation_id: RepresentationId,
) -> Result<RepresentationFingerprint, CatalogError> {
    representation_fingerprint(transaction, representation_id)
}

fn upsert_snapshot(
    transaction: &Transaction<'_>,
    request: &RecordDecodeSnapshot,
    snapshot_json: &str,
) -> Result<(), CatalogError> {
    let capabilities = &request.snapshot.capabilities;
    transaction.execute(
        "INSERT INTO representation_decode_snapshots(
             representation_id, provider_id, provider_version, snapshot_schema, snapshot_json,
             source_byte_len, source_modified_at_ms, inspected_at_ms, has_metadata,
             has_embedded_previews, can_decode_mosaic, can_render_reference_rgb,
             has_pending_corrections
         ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13)
         ON CONFLICT(representation_id, provider_id) DO UPDATE SET
             provider_version = excluded.provider_version,
             snapshot_schema = excluded.snapshot_schema,
             snapshot_json = excluded.snapshot_json,
             source_byte_len = excluded.source_byte_len,
             source_modified_at_ms = excluded.source_modified_at_ms,
             inspected_at_ms = excluded.inspected_at_ms,
             has_metadata = excluded.has_metadata,
             has_embedded_previews = excluded.has_embedded_previews,
             can_decode_mosaic = excluded.can_decode_mosaic,
             can_render_reference_rgb = excluded.can_render_reference_rgb,
             has_pending_corrections = excluded.has_pending_corrections",
        params![
            request.representation_id.as_bytes().as_slice(),
            request.snapshot.provider.id,
            request.snapshot.provider.version,
            SNAPSHOT_SCHEMA,
            snapshot_json,
            sqlite_u64(request.expected_source.byte_len, "source_byte_len")?,
            request.expected_source.modified_at_ms,
            request.inspected_at_ms,
            sqlite_bool(capabilities.metadata.is_available()),
            sqlite_bool(capabilities.embedded_previews.is_available()),
            sqlite_bool(capabilities.mosaic.is_available()),
            sqlite_bool(capabilities.reference_rgb.is_available()),
            sqlite_bool(capabilities.pending_corrections.has_pending()),
        ],
    )?;
    Ok(())
}

fn replace_previews(
    transaction: &Transaction<'_>,
    request: &RecordDecodeSnapshot,
) -> Result<(), CatalogError> {
    transaction.execute(
        "DELETE FROM representation_previews
         WHERE representation_id = ?1 AND provider_id = ?2",
        params![
            request.representation_id.as_bytes().as_slice(),
            request.snapshot.provider.id
        ],
    )?;

    let mut statement = transaction.prepare_cached(
        "INSERT INTO representation_previews(
             representation_id, provider_id, provider_preview_id, codec, width, height,
             bits_per_channel, channels, encoded_bytes, decodable
         ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10)",
    )?;
    for preview in &request.snapshot.previews {
        statement.execute(params![
            request.representation_id.as_bytes().as_slice(),
            request.snapshot.provider.id,
            sqlite_usize(preview.provider_id, "provider_preview_id")?,
            preview.codec.as_str(),
            i64::from(preview.dimensions.width),
            i64::from(preview.dimensions.height),
            i64::from(preview.bits_per_channel),
            i64::from(preview.channels),
            sqlite_u64(preview.encoded_bytes, "encoded_bytes")?,
            sqlite_bool(preview.decodable),
        ])?;
    }
    Ok(())
}

fn sqlite_bool(value: bool) -> i64 {
    i64::from(value)
}

fn sqlite_u64(value: u64, field: &'static str) -> Result<i64, CatalogError> {
    i64::try_from(value).map_err(|_| CatalogError::DecodeSnapshotValueOutOfRange { field })
}

fn sqlite_usize(value: usize, field: &'static str) -> Result<i64, CatalogError> {
    i64::try_from(value).map_err(|_| CatalogError::DecodeSnapshotValueOutOfRange { field })
}

fn non_negative_u64(value: i64, index: usize) -> rusqlite::Result<u64> {
    u64::try_from(value).map_err(|error| {
        rusqlite::Error::FromSqlConversionFailure(index, Type::Integer, Box::new(error))
    })
}

#[cfg(test)]
mod tests {
    use shadow_ai::{DISPLAY_LUMA_CONTRACT_VERSION, DisplayLumaPlane, observe_display_luma};
    use shadow_domain::{
        AssetLocation, DecodeCapabilitySnapshot, DecodeProviderSnapshot, DecodeSupport,
        ImageDimensions, ImageMargins, PendingCorrectionsSnapshot, Platform, PreviewCodec,
        PreviewDescriptorSnapshot, RawMetadataSnapshot, RepresentationKind,
    };

    use super::*;
    use crate::{
        CachedArtifact, CachedArtifactRole, RecordCachedArtifact, RecordTechnicalObservation,
        RegisterAsset, RegistrationStatus, technical_observation::artifact_content_hash,
    };

    fn registered_catalog() -> (Catalog, RepresentationId, RepresentationFingerprint) {
        let mut catalog = Catalog::open_in_memory().expect("open catalog");
        let source = RepresentationFingerprint {
            byte_len: 1_024,
            modified_at_ms: Some(123),
        };
        let registered = catalog
            .register_asset(&RegisterAsset {
                kind: RepresentationKind::OriginalRaw,
                location: AssetLocation::new(
                    Platform::MacOs,
                    b"/photos/input.dng".to_vec(),
                    "/photos/input.dng",
                ),
                byte_len: source.byte_len,
                modified_at_ms: source.modified_at_ms,
                now_ms: 100,
            })
            .expect("register asset");
        assert_eq!(registered.status, RegistrationStatus::Inserted);
        (catalog, registered.representation_id, source)
    }

    fn snapshot(provider_id: &str, version: &str, preview_ids: &[usize]) -> DecoderSnapshot {
        DecoderSnapshot {
            provider: DecodeProviderSnapshot {
                id: provider_id.into(),
                version: version.into(),
                dng_sdk: false,
                rawspeed: false,
                jpeg: true,
            },
            metadata: RawMetadataSnapshot {
                make: "Pentax".into(),
                model: "K10D".into(),
                normalized_make: "Pentax".into(),
                normalized_model: "K10D".into(),
                dng_version: Some("1.1.0.0".into()),
                raw_count: 1,
                raw_dimensions: ImageDimensions {
                    width: 3_936,
                    height: 2_624,
                },
                image_dimensions: ImageDimensions {
                    width: 3_896,
                    height: 2_616,
                },
                margins: ImageMargins::default(),
                orientation: 0,
                cfa_pattern: "RGGB".into(),
                sensor_colors: 3,
                sensor_bits: 12,
                black_level: 0,
                white_level: 4_095,
                as_shot_neutral: [0.64, 1.0, 0.966, 0.0],
                baseline_exposure: -0.5,
                iso_speed: 100.0,
                exposure_time_seconds: 1.0 / 125.0,
                aperture_f_number: 5.6,
                focal_length_mm: 35.0,
                captured_at_unix_seconds: 1_700_000_000,
                lens_make: "Pentax".into(),
                lens_model: "smc PENTAX-DA 35mm".into(),
                focal_length_35mm: 52.0,
            },
            capabilities: DecodeCapabilitySnapshot {
                metadata: DecodeSupport::Available,
                embedded_previews: DecodeSupport::Available,
                mosaic: DecodeSupport::Available,
                reference_rgb: DecodeSupport::Available,
                pending_corrections: PendingCorrectionsSnapshot::default(),
            },
            previews: preview_ids
                .iter()
                .map(|provider_id| PreviewDescriptorSnapshot {
                    provider_id: *provider_id,
                    codec: PreviewCodec::Jpeg,
                    dimensions: ImageDimensions {
                        width: 3_872,
                        height: 2_592,
                    },
                    bits_per_channel: 8,
                    channels: 3,
                    encoded_bytes: 1_285_213,
                    decodable: true,
                })
                .collect(),
        }
    }

    #[test]
    fn snapshot_round_trips_with_source_identity() {
        let (mut catalog, representation_id, source) = registered_catalog();
        let snapshot = snapshot("libraw", "0.22.2", &[1, 2]);
        let status = catalog
            .record_decode_snapshot(&RecordDecodeSnapshot {
                representation_id,
                expected_source: source,
                snapshot: snapshot.clone(),
                inspected_at_ms: 456,
            })
            .expect("record snapshot");

        assert_eq!(status, RecordDecodeSnapshotStatus::Recorded);
        assert_eq!(
            catalog.decode_snapshots(representation_id).expect("read"),
            vec![DecodeSnapshotRecord {
                representation_id,
                source,
                inspected_at_ms: 456,
                snapshot,
            }]
        );

        let photo_id = catalog
            .connection
            .query_row(
                "SELECT photo_id FROM representations WHERE id = ?1",
                [representation_id.as_bytes().as_slice()],
                |row| read_id(row, 0),
            )
            .expect("read owner photo");
        let facts = catalog
            .photo_library_facts(photo_id)
            .expect("read projected Library facts")
            .expect("metadata projection");
        assert_eq!(facts.capture_day, "2023-11-14");
        assert_eq!(facts.camera_make, "Pentax");
        assert_eq!(facts.camera_model, "K10D");
        assert_eq!(facts.lens_model, "smc PENTAX-DA 35mm");
        assert_eq!(facts.aperture_milli, Some(5_600));
        assert_eq!(facts.focal_length_tenth_mm, Some(350));
        assert_eq!(facts.iso_speed, Some(100.0));
        assert_eq!(facts.indexed_representation_id, Some(representation_id));
        assert_eq!(facts.indexed_source, Some(source));
        assert_eq!(facts.indexed_at_ms, 456);
    }

    #[test]
    fn same_provider_replaces_snapshot_and_preview_rows() {
        let (mut catalog, representation_id, source) = registered_catalog();
        for (version, preview_ids) in [("0.22.1", &[1, 2][..]), ("0.22.2", &[7][..])] {
            catalog
                .record_decode_snapshot(&RecordDecodeSnapshot {
                    representation_id,
                    expected_source: source,
                    snapshot: snapshot("libraw", version, preview_ids),
                    inspected_at_ms: 456,
                })
                .expect("record snapshot");
        }

        let records = catalog.decode_snapshots(representation_id).expect("read");
        assert_eq!(records.len(), 1);
        assert_eq!(records[0].snapshot.provider.version, "0.22.2");
        assert_eq!(records[0].snapshot.previews[0].provider_id, 7);
        let preview_rows: i64 = catalog
            .connection
            .query_row("SELECT COUNT(*) FROM representation_previews", [], |row| {
                row.get(0)
            })
            .expect("count previews");
        assert_eq!(preview_rows, 1);
    }

    #[test]
    fn distinct_providers_coexist() {
        let (mut catalog, representation_id, source) = registered_catalog();
        for provider_id in ["libraw", "nikon-private"] {
            catalog
                .record_decode_snapshot(&RecordDecodeSnapshot {
                    representation_id,
                    expected_source: source,
                    snapshot: snapshot(provider_id, "1", &[]),
                    inspected_at_ms: 456,
                })
                .expect("record snapshot");
        }

        let records = catalog.decode_snapshots(representation_id).expect("read");
        assert_eq!(records.len(), 2);
        assert_eq!(records[0].snapshot.provider.id, "libraw");
        assert_eq!(records[1].snapshot.provider.id, "nikon-private");
    }

    #[test]
    fn stale_source_cannot_overwrite_current_snapshot() {
        let (mut catalog, representation_id, source) = registered_catalog();
        let first = snapshot("libraw", "current", &[1]);
        catalog
            .record_decode_snapshot(&RecordDecodeSnapshot {
                representation_id,
                expected_source: source,
                snapshot: first.clone(),
                inspected_at_ms: 456,
            })
            .expect("record current snapshot");

        let status = catalog
            .record_decode_snapshot(&RecordDecodeSnapshot {
                representation_id,
                expected_source: RepresentationFingerprint {
                    byte_len: source.byte_len + 1,
                    modified_at_ms: source.modified_at_ms,
                },
                snapshot: snapshot("libraw", "stale", &[2]),
                inspected_at_ms: 789,
            })
            .expect("reject stale snapshot");

        assert_eq!(status, RecordDecodeSnapshotStatus::StaleSource);
        let records = catalog.decode_snapshots(representation_id).expect("read");
        assert_eq!(records[0].snapshot, first);
    }

    #[test]
    fn non_finite_metadata_is_rejected_before_json_persistence() {
        let (mut catalog, representation_id, source) = registered_catalog();
        let mut invalid = snapshot("libraw", "invalid", &[]);
        invalid.metadata.baseline_exposure = f64::NAN;

        let error = catalog
            .record_decode_snapshot(&RecordDecodeSnapshot {
                representation_id,
                expected_source: source,
                snapshot: invalid,
                inspected_at_ms: 456,
            })
            .expect_err("reject non-finite snapshot");
        assert!(matches!(error, CatalogError::InvalidDecodeSnapshot(_)));
        assert!(
            catalog
                .decode_snapshots(representation_id)
                .expect("read snapshots")
                .is_empty()
        );
    }

    #[test]
    #[allow(clippy::too_many_lines)]
    fn cached_preview_requirement_reconciles_missing_artifacts() {
        const PROXY_KEY: &str = "libraw:grid-jpeg-2048-q88-444-v3";
        let (mut catalog, representation_id, source) = registered_catalog();
        catalog
            .record_decode_snapshot(&RecordDecodeSnapshot {
                representation_id,
                expected_source: source,
                snapshot: snapshot("libraw", "1", &[7]),
                inspected_at_ms: 456,
            })
            .expect("record snapshot");

        assert!(
            catalog
                .is_decode_output_current(
                    representation_id,
                    "libraw",
                    "1",
                    source,
                    false,
                    PROXY_KEY,
                    None,
                )
                .expect("query descriptor-only state")
        );
        assert!(
            !catalog
                .is_decode_output_current(
                    representation_id,
                    "libraw",
                    "2",
                    source,
                    false,
                    PROXY_KEY,
                    None,
                )
                .expect("query newer provider version")
        );
        assert!(
            !catalog
                .is_decode_output_current(
                    representation_id,
                    "libraw",
                    "1",
                    source,
                    true,
                    PROXY_KEY,
                    None,
                )
                .expect("query missing cached preview")
        );

        let artifact = CachedArtifact {
            role: CachedArtifactRole::EmbeddedPreview,
            variant_key: "libraw".into(),
            generator_id: "libraw".into(),
            generator_version: "1".into(),
            provider_preview_id: Some(7),
            blob_algorithm: "blake3-256".into(),
            blob_digest: [1; 32],
            blob_byte_len: 1_024,
            codec: PreviewCodec::Jpeg,
            byte_order: shadow_domain::PreviewByteOrder::NotApplicable,
            dimensions: ImageDimensions {
                width: 1_600,
                height: 1_200,
            },
            bits_per_channel: 8,
            channels: 3,
            created_at_ms: 789,
        };
        catalog
            .record_cached_artifact(&RecordCachedArtifact {
                representation_id,
                expected_source: source,
                artifact: artifact.clone(),
            })
            .expect("record cached preview");

        assert!(
            catalog
                .is_decode_output_current(
                    representation_id,
                    "libraw",
                    "1",
                    source,
                    true,
                    PROXY_KEY,
                    None,
                )
                .expect("query complete cached state")
        );
        assert!(
            !catalog
                .is_decode_output_current(
                    representation_id,
                    "libraw",
                    "1",
                    source,
                    true,
                    PROXY_KEY,
                    Some("jpeg-luma-v1"),
                )
                .expect("migration backfill requires technical observation")
        );
        record_technical(
            &mut catalog,
            representation_id,
            source,
            &artifact,
            "jpeg-luma-v1",
        );
        assert!(
            catalog
                .is_decode_output_current(
                    representation_id,
                    "libraw",
                    "1",
                    source,
                    true,
                    PROXY_KEY,
                    Some("jpeg-luma-v1"),
                )
                .expect("matching technical observation completes backfill")
        );
        catalog
            .connection
            .execute(
                "UPDATE representation_technical_observations
                 SET observation_digest = zeroblob(32)
                 WHERE preprocessing_version = 'jpeg-luma-v1'",
                [],
            )
            .expect("corrupt rebuildable technical observation");
        assert!(
            !catalog
                .is_decode_output_current(
                    representation_id,
                    "libraw",
                    "1",
                    source,
                    true,
                    PROXY_KEY,
                    Some("jpeg-luma-v1"),
                )
                .expect("corrupt observation requires regeneration")
        );
        assert!(
            !catalog
                .is_decode_output_current(
                    representation_id,
                    "libraw",
                    "1",
                    source,
                    true,
                    PROXY_KEY,
                    Some("jpeg-luma-v2"),
                )
                .expect("preprocessing upgrade requires backfill")
        );
    }

    #[test]
    #[allow(clippy::too_many_lines)]
    fn generated_proxy_requirement_uses_the_current_recipe_and_provider_version() {
        const PROXY_KEY: &str = "libraw:grid-jpeg-2048-q88-444-v3";
        let (mut catalog, representation_id, source) = registered_catalog();
        let mut without_preview = snapshot("libraw", "1", &[]);
        without_preview.capabilities.embedded_previews = DecodeSupport::Unavailable;
        catalog
            .record_decode_snapshot(&RecordDecodeSnapshot {
                representation_id,
                expected_source: source,
                snapshot: without_preview,
                inspected_at_ms: 456,
            })
            .expect("record snapshot");

        assert!(
            !catalog
                .is_decode_output_current(
                    representation_id,
                    "libraw",
                    "1",
                    source,
                    true,
                    PROXY_KEY,
                    None,
                )
                .expect("query missing generated proxy")
        );

        catalog
            .record_cached_artifact(&RecordCachedArtifact {
                representation_id,
                expected_source: source,
                artifact: CachedArtifact {
                    role: CachedArtifactRole::GeneratedProxy,
                    variant_key: PROXY_KEY.into(),
                    generator_id: "libraw".into(),
                    generator_version: "stale".into(),
                    provider_preview_id: None,
                    blob_algorithm: "blake3-256".into(),
                    blob_digest: [1; 32],
                    blob_byte_len: 456_789,
                    codec: PreviewCodec::Jpeg,
                    byte_order: shadow_domain::PreviewByteOrder::NotApplicable,
                    dimensions: ImageDimensions {
                        width: 2_048,
                        height: 1_365,
                    },
                    bits_per_channel: 8,
                    channels: 3,
                    created_at_ms: 788,
                },
            })
            .expect("record generated proxy from stale provider version");

        assert!(
            !catalog
                .is_decode_output_current(
                    representation_id,
                    "libraw",
                    "1",
                    source,
                    true,
                    PROXY_KEY,
                    None,
                )
                .expect("generator version mismatch must not hit")
        );

        catalog
            .record_cached_artifact(&RecordCachedArtifact {
                representation_id,
                expected_source: source,
                artifact: CachedArtifact {
                    role: CachedArtifactRole::GeneratedProxy,
                    variant_key: PROXY_KEY.into(),
                    generator_id: "libraw".into(),
                    generator_version: "1".into(),
                    provider_preview_id: None,
                    blob_algorithm: "blake3-256".into(),
                    blob_digest: [2; 32],
                    blob_byte_len: 456_789,
                    codec: PreviewCodec::Jpeg,
                    byte_order: shadow_domain::PreviewByteOrder::NotApplicable,
                    dimensions: ImageDimensions {
                        width: 2_048,
                        height: 1_365,
                    },
                    bits_per_channel: 8,
                    channels: 3,
                    created_at_ms: 789,
                },
            })
            .expect("record generated proxy");

        assert!(
            catalog
                .is_decode_output_current(
                    representation_id,
                    "libraw",
                    "1",
                    source,
                    true,
                    PROXY_KEY,
                    None,
                )
                .expect("query complete generated proxy")
        );
        assert!(
            !catalog
                .is_decode_output_current(
                    representation_id,
                    "libraw",
                    "1",
                    source,
                    true,
                    "libraw:grid-jpeg-2048-q88-444-v4",
                    None,
                )
                .expect("query newer proxy recipe")
        );
    }

    #[test]
    fn non_jpeg_visual_does_not_create_a_technical_backfill_loop() {
        const PROXY_KEY: &str = "libraw:grid-bitmap-2048-v1";
        let (mut catalog, representation_id, source) = registered_catalog();
        let mut without_preview = snapshot("libraw", "1", &[]);
        without_preview.capabilities.embedded_previews = DecodeSupport::Unavailable;
        catalog
            .record_decode_snapshot(&RecordDecodeSnapshot {
                representation_id,
                expected_source: source,
                snapshot: without_preview,
                inspected_at_ms: 456,
            })
            .expect("record snapshot");
        catalog
            .record_cached_artifact(&RecordCachedArtifact {
                representation_id,
                expected_source: source,
                artifact: CachedArtifact {
                    role: CachedArtifactRole::GeneratedProxy,
                    variant_key: PROXY_KEY.into(),
                    generator_id: "libraw".into(),
                    generator_version: "1".into(),
                    provider_preview_id: None,
                    blob_algorithm: "blake3-256".into(),
                    blob_digest: [3; 32],
                    blob_byte_len: 456_789,
                    codec: PreviewCodec::Bitmap,
                    byte_order: shadow_domain::PreviewByteOrder::Native,
                    dimensions: ImageDimensions {
                        width: 2_048,
                        height: 1_365,
                    },
                    bits_per_channel: 8,
                    channels: 3,
                    created_at_ms: 790,
                },
            })
            .expect("record non-JPEG visual");
        assert!(
            catalog
                .is_decode_output_current(
                    representation_id,
                    "libraw",
                    "1",
                    source,
                    true,
                    PROXY_KEY,
                    Some("jpeg-luma-v1"),
                )
                .expect("non-JPEG visuals do not create an impossible backfill loop")
        );
    }

    fn record_technical(
        catalog: &mut Catalog,
        representation_id: RepresentationId,
        source: RepresentationFingerprint,
        artifact: &CachedArtifact,
        preprocessing_version: &str,
    ) {
        let input_source_hash = artifact_content_hash(artifact);
        let observation = observe_display_luma(DisplayLumaPlane {
            contract_version: DISPLAY_LUMA_CONTRACT_VERSION,
            width: 2,
            height: 2,
            stride: 2,
            samples: &[0.0, 0.25, 0.75, 1.0],
            preprocessing_version,
            input_source_hash: &input_source_hash,
        })
        .expect("observe luma");
        catalog
            .record_technical_observation(&RecordTechnicalObservation {
                representation_id,
                expected_source: source,
                expected_artifact: artifact.clone(),
                observation,
                observed_at_ms: 900,
            })
            .expect("record technical observation");
    }
}
