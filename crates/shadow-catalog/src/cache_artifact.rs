use std::cmp::Ordering;

use rusqlite::{OptionalExtension, params, types::Type};
use shadow_domain::{EntityId, ImageDimensions, PreviewByteOrder, PreviewCodec, RepresentationId};

use crate::{
    Catalog, CatalogError, RepresentationFingerprint,
    decode_snapshot::representation_fingerprint_in_transaction, read_id,
};

#[derive(Debug, Copy, Clone, Eq, PartialEq, Hash)]
pub enum CachedArtifactRole {
    /// A rendered JPEG bound to one exact durable working Recipe snapshot.
    RecipePreview,
    EmbeddedPreview,
    GeneratedProxy,
}

impl CachedArtifactRole {
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::RecipePreview => "recipe_preview",
            Self::EmbeddedPreview => "embedded_preview",
            Self::GeneratedProxy => "generated_proxy",
        }
    }
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct CachedArtifact {
    pub role: CachedArtifactRole,
    pub variant_key: String,
    pub generator_id: String,
    pub generator_version: String,
    /// Present only for an edited render. It is the exact Recipe snapshot
    /// identity that must still be named by the photo's `working` ref before
    /// this artifact is eligible for the Library grid.
    pub recipe_snapshot_digest: Option<[u8; 32]>,
    pub provider_preview_id: Option<usize>,
    pub blob_algorithm: String,
    pub blob_digest: [u8; 32],
    pub blob_byte_len: u64,
    pub codec: PreviewCodec,
    pub byte_order: PreviewByteOrder,
    pub dimensions: ImageDimensions,
    pub bits_per_channel: u16,
    pub channels: u16,
    pub created_at_ms: i64,
}

/// The exact implementation identity required for a cached artifact consumer.
///
/// Artifact rows remain rebuildable history. Consumers use this value to
/// reject output produced by an incompatible generator without deleting the
/// older row or coupling that policy to the Catalog schema.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct CachedArtifactGeneratorIdentity {
    pub generator_id: String,
    pub generator_version: String,
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct RecordCachedArtifact {
    pub representation_id: RepresentationId,
    pub expected_source: RepresentationFingerprint,
    pub artifact: CachedArtifact,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub enum RecordCachedArtifactStatus {
    Recorded,
    StaleSource,
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct CachedArtifactRecord {
    pub representation_id: RepresentationId,
    pub source: RepresentationFingerprint,
    pub artifact: CachedArtifact,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub enum InvalidateCachedArtifactStatus {
    Invalidated,
    NotCurrent,
}

impl Catalog {
    /// Atomically records a reference to one already-written cache blob.
    ///
    /// The cache file is rebuildable and lives outside `SQLite`. This method only
    /// commits the content identity and image semantics when the representation
    /// still matches the source revision used to produce it.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] for invalid metadata, a missing representation,
    /// integer overflow, or a failed transaction.
    pub fn record_cached_artifact(
        &mut self,
        request: &RecordCachedArtifact,
    ) -> Result<RecordCachedArtifactStatus, CatalogError> {
        validate_artifact(&request.artifact)?;
        let transaction = self.connection.transaction()?;
        let current =
            representation_fingerprint_in_transaction(&transaction, request.representation_id)?;
        if current != request.expected_source {
            return Ok(RecordCachedArtifactStatus::StaleSource);
        }

        let artifact = &request.artifact;
        transaction.execute(
            "INSERT INTO representation_cached_artifacts(
                 representation_id, role, variant_key, generator_id, generator_version,
                 recipe_snapshot_digest, provider_preview_id, source_byte_len,
                 source_modified_at_ms, blob_algorithm, blob_digest, blob_byte_len, codec,
                 byte_order, width, height, bits_per_channel, channels, created_at_ms
             ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13,
                       ?14, ?15, ?16, ?17, ?18, ?19)
             ON CONFLICT(representation_id, role, variant_key) DO UPDATE SET
                 generator_id = excluded.generator_id,
                 generator_version = excluded.generator_version,
                 recipe_snapshot_digest = excluded.recipe_snapshot_digest,
                 provider_preview_id = excluded.provider_preview_id,
                 source_byte_len = excluded.source_byte_len,
                 source_modified_at_ms = excluded.source_modified_at_ms,
                 blob_algorithm = excluded.blob_algorithm,
                 blob_digest = excluded.blob_digest,
                 blob_byte_len = excluded.blob_byte_len,
                 codec = excluded.codec,
                 byte_order = excluded.byte_order,
                 width = excluded.width,
                 height = excluded.height,
                 bits_per_channel = excluded.bits_per_channel,
                 channels = excluded.channels,
                 created_at_ms = excluded.created_at_ms",
            params![
                request.representation_id.as_bytes().as_slice(),
                artifact.role.as_str(),
                artifact.variant_key,
                artifact.generator_id,
                artifact.generator_version,
                artifact
                    .recipe_snapshot_digest
                    .as_ref()
                    .map(|digest| digest.as_slice()),
                artifact
                    .provider_preview_id
                    .map(|value| sqlite_usize(value, "provider_preview_id"))
                    .transpose()?,
                sqlite_u64(request.expected_source.byte_len, "source_byte_len")?,
                request.expected_source.modified_at_ms,
                artifact.blob_algorithm,
                artifact.blob_digest.as_slice(),
                sqlite_u64(artifact.blob_byte_len, "blob_byte_len")?,
                artifact.codec.as_str(),
                artifact.byte_order.as_str(),
                i64::from(artifact.dimensions.width),
                i64::from(artifact.dimensions.height),
                i64::from(artifact.bits_per_channel),
                i64::from(artifact.channels),
                artifact.created_at_ms,
            ],
        )?;
        transaction.commit()?;
        Ok(RecordCachedArtifactStatus::Recorded)
    }

    /// Lists cached artifacts for a representation in stable role/key order.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the representation is absent, persisted
    /// enum text is unknown, numeric data is invalid, or the query fails.
    pub fn cached_artifacts(
        &self,
        representation_id: RepresentationId,
    ) -> Result<Vec<CachedArtifactRecord>, CatalogError> {
        self.representation_fingerprint(representation_id)?;
        let mut statement = self.connection.prepare(
            "SELECT representation_id, role, variant_key, generator_id, generator_version,
                    recipe_snapshot_digest, provider_preview_id, source_byte_len,
                    source_modified_at_ms, blob_algorithm, blob_digest, blob_byte_len, codec,
                    byte_order, width, height, bits_per_channel, channels, created_at_ms
             FROM representation_cached_artifacts
             WHERE representation_id = ?1
             ORDER BY role, variant_key",
        )?;
        let rows = statement.query_map([representation_id.as_bytes().as_slice()], |row| {
            Ok((
                read_id(row, 0)?,
                row.get::<_, String>(1)?,
                row.get::<_, String>(2)?,
                row.get::<_, String>(3)?,
                row.get::<_, String>(4)?,
                row.get::<_, Option<Vec<u8>>>(5)?
                    .map(|value| digest(value, 5))
                    .transpose()?,
                optional_usize(row.get::<_, Option<i64>>(6)?, 6)?,
                non_negative_u64(row.get(7)?, 7)?,
                row.get::<_, Option<i64>>(8)?,
                row.get::<_, String>(9)?,
                digest(row.get(10)?, 10)?,
                non_negative_u64(row.get(11)?, 11)?,
                row.get::<_, String>(12)?,
                row.get::<_, String>(13)?,
                non_negative_u32(row.get(14)?, 14)?,
                non_negative_u32(row.get(15)?, 15)?,
                non_negative_u16(row.get(16)?, 16)?,
                non_negative_u16(row.get(17)?, 17)?,
                row.get::<_, i64>(18)?,
            ))
        })?;

        let mut artifacts = Vec::new();
        for row in rows {
            let (
                stored_representation_id,
                role,
                variant_key,
                generator_id,
                generator_version,
                recipe_snapshot_digest,
                provider_preview_id,
                byte_len,
                modified_at_ms,
                blob_algorithm,
                blob_digest,
                blob_byte_len,
                codec,
                byte_order,
                width,
                height,
                bits_per_channel,
                channels,
                created_at_ms,
            ) = row?;
            artifacts.push(CachedArtifactRecord {
                representation_id: stored_representation_id,
                source: RepresentationFingerprint {
                    byte_len,
                    modified_at_ms,
                },
                artifact: CachedArtifact {
                    role: parse_role(role)?,
                    variant_key,
                    generator_id,
                    generator_version,
                    recipe_snapshot_digest,
                    provider_preview_id,
                    blob_algorithm,
                    blob_digest,
                    blob_byte_len,
                    codec: parse_codec(codec)?,
                    byte_order: parse_byte_order(byte_order)?,
                    dimensions: ImageDimensions { width, height },
                    bits_per_channel,
                    channels,
                    created_at_ms,
                },
            });
        }
        Ok(artifacts)
    }

    /// Returns the exact current visual selected by the shared Review ordering.
    ///
    /// A current Recipe preview precedes Shadow-generated proxies, which
    /// precede camera-embedded placeholders. Within a role the largest and
    /// newest image wins, followed by the stable variant key. Keeping this
    /// selection in Catalog prevents background analysis and Review from
    /// targeting different provider/variant artifacts.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the representation is absent or persisted
    /// artifact metadata is invalid.
    pub fn preferred_cached_artifact(
        &self,
        representation_id: RepresentationId,
    ) -> Result<Option<CachedArtifactRecord>, CatalogError> {
        let source = self.representation_fingerprint(representation_id)?;
        let working_recipe_snapshot = self
            .connection
            .query_row(
                "SELECT c.snapshot_digest
                 FROM representations r
                 JOIN recipe_refs rr ON rr.photo_id = r.photo_id AND rr.name = 'working'
                 JOIN recipe_commits c ON c.id = rr.commit_id AND c.photo_id = rr.photo_id
                 WHERE r.id = ?1",
                [representation_id.as_bytes().as_slice()],
                |row| digest(row.get(0)?, 0),
            )
            .optional()?;
        let mut artifacts = self.cached_artifacts(representation_id)?;
        artifacts.retain(|record| {
            record.source == source
                && (record.artifact.role != CachedArtifactRole::RecipePreview
                    || record.artifact.recipe_snapshot_digest == working_recipe_snapshot)
        });
        artifacts.sort_by(preferred_artifact_ordering);
        Ok(artifacts.into_iter().next())
    }

    /// Removes an artifact reference only if the Catalog row is still exactly
    /// the record observed by a failed cache read.
    ///
    /// The full provenance, source fingerprint, blob identity, and creation
    /// timestamp prevent a slow reader from deleting a replacement committed by
    /// another worker after that reader loaded its record.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] for numeric overflow or a failed delete.
    pub fn invalidate_cached_artifact(
        &mut self,
        record: &CachedArtifactRecord,
    ) -> Result<InvalidateCachedArtifactStatus, CatalogError> {
        let artifact = &record.artifact;
        let deleted = self.connection.execute(
            "DELETE FROM representation_cached_artifacts
             WHERE representation_id = ?1 AND role = ?2 AND variant_key = ?3
               AND generator_id = ?4 AND generator_version = ?5
               AND recipe_snapshot_digest IS ?6
               AND source_byte_len = ?7 AND source_modified_at_ms IS ?8
               AND blob_algorithm = ?9 AND blob_digest = ?10 AND blob_byte_len = ?11
               AND created_at_ms = ?12",
            params![
                record.representation_id.as_bytes().as_slice(),
                artifact.role.as_str(),
                artifact.variant_key,
                artifact.generator_id,
                artifact.generator_version,
                artifact
                    .recipe_snapshot_digest
                    .as_ref()
                    .map(|digest| digest.as_slice()),
                sqlite_u64(record.source.byte_len, "source_byte_len")?,
                record.source.modified_at_ms,
                artifact.blob_algorithm,
                artifact.blob_digest.as_slice(),
                sqlite_u64(artifact.blob_byte_len, "blob_byte_len")?,
                artifact.created_at_ms,
            ],
        )?;
        Ok(if deleted == 1 {
            InvalidateCachedArtifactStatus::Invalidated
        } else {
            InvalidateCachedArtifactStatus::NotCurrent
        })
    }
}

fn preferred_artifact_ordering(
    left: &CachedArtifactRecord,
    right: &CachedArtifactRecord,
) -> Ordering {
    artifact_role_rank(left.artifact.role)
        .cmp(&artifact_role_rank(right.artifact.role))
        .then_with(|| artifact_area(right).cmp(&artifact_area(left)))
        .then_with(|| {
            right
                .artifact
                .created_at_ms
                .cmp(&left.artifact.created_at_ms)
        })
        .then_with(|| left.artifact.variant_key.cmp(&right.artifact.variant_key))
}

const fn artifact_role_rank(role: CachedArtifactRole) -> u8 {
    match role {
        CachedArtifactRole::RecipePreview => 0,
        CachedArtifactRole::GeneratedProxy => 1,
        CachedArtifactRole::EmbeddedPreview => 2,
    }
}

fn artifact_area(record: &CachedArtifactRecord) -> u64 {
    u64::from(record.artifact.dimensions.width) * u64::from(record.artifact.dimensions.height)
}

fn validate_artifact(artifact: &CachedArtifact) -> Result<(), CatalogError> {
    if artifact.variant_key.is_empty() {
        return Err(CatalogError::InvalidCachedArtifact(
            "variant key must not be empty",
        ));
    }
    if artifact.generator_id.is_empty() {
        return Err(CatalogError::InvalidCachedArtifact(
            "generator id must not be empty",
        ));
    }
    if artifact.blob_algorithm.is_empty() || artifact.blob_byte_len == 0 {
        return Err(CatalogError::InvalidCachedArtifact(
            "blob identity and length must be present",
        ));
    }
    match (artifact.role, artifact.recipe_snapshot_digest.is_some()) {
        (CachedArtifactRole::RecipePreview, true)
        | (CachedArtifactRole::EmbeddedPreview | CachedArtifactRole::GeneratedProxy, false) => {}
        (CachedArtifactRole::RecipePreview, false) => {
            return Err(CatalogError::InvalidCachedArtifact(
                "recipe preview must name an exact Recipe snapshot",
            ));
        }
        (CachedArtifactRole::EmbeddedPreview | CachedArtifactRole::GeneratedProxy, true) => {
            return Err(CatalogError::InvalidCachedArtifact(
                "source preview must not carry a Recipe snapshot",
            ));
        }
    }
    Ok(())
}

pub(crate) fn parse_role(value: String) -> Result<CachedArtifactRole, CatalogError> {
    match value.as_str() {
        "recipe_preview" => Ok(CachedArtifactRole::RecipePreview),
        "embedded_preview" => Ok(CachedArtifactRole::EmbeddedPreview),
        "generated_proxy" => Ok(CachedArtifactRole::GeneratedProxy),
        _ => Err(unknown("role", value)),
    }
}

pub(crate) fn parse_codec(value: String) -> Result<PreviewCodec, CatalogError> {
    match value.as_str() {
        "unknown" => Ok(PreviewCodec::Unknown),
        "jpeg" => Ok(PreviewCodec::Jpeg),
        "bitmap" => Ok(PreviewCodec::Bitmap),
        "jpeg_xl" => Ok(PreviewCodec::JpegXl),
        "h265" => Ok(PreviewCodec::H265),
        _ => Err(unknown("codec", value)),
    }
}

pub(crate) fn parse_byte_order(value: String) -> Result<PreviewByteOrder, CatalogError> {
    match value.as_str() {
        "not_applicable" => Ok(PreviewByteOrder::NotApplicable),
        "native" => Ok(PreviewByteOrder::Native),
        "little_endian" => Ok(PreviewByteOrder::LittleEndian),
        "big_endian" => Ok(PreviewByteOrder::BigEndian),
        _ => Err(unknown("byte_order", value)),
    }
}

fn unknown(field: &'static str, value: String) -> CatalogError {
    CatalogError::UnknownCachedArtifactValue { field, value }
}

fn sqlite_u64(value: u64, field: &'static str) -> Result<i64, CatalogError> {
    i64::try_from(value).map_err(|_| CatalogError::CachedArtifactValueOutOfRange { field })
}

fn sqlite_usize(value: usize, field: &'static str) -> Result<i64, CatalogError> {
    i64::try_from(value).map_err(|_| CatalogError::CachedArtifactValueOutOfRange { field })
}

pub(crate) fn non_negative_u64(value: i64, index: usize) -> rusqlite::Result<u64> {
    u64::try_from(value).map_err(|error| conversion(index, error))
}

pub(crate) fn non_negative_u32(value: i64, index: usize) -> rusqlite::Result<u32> {
    u32::try_from(value).map_err(|error| conversion(index, error))
}

pub(crate) fn non_negative_u16(value: i64, index: usize) -> rusqlite::Result<u16> {
    u16::try_from(value).map_err(|error| conversion(index, error))
}

pub(crate) fn optional_usize(value: Option<i64>, index: usize) -> rusqlite::Result<Option<usize>> {
    value
        .map(|value| usize::try_from(value).map_err(|error| conversion(index, error)))
        .transpose()
}

pub(crate) fn digest(value: Vec<u8>, index: usize) -> rusqlite::Result<[u8; 32]> {
    value.try_into().map_err(|value: Vec<u8>| {
        rusqlite::Error::FromSqlConversionFailure(
            index,
            Type::Blob,
            Box::new(std::io::Error::new(
                std::io::ErrorKind::InvalidData,
                format!("expected 32 digest bytes, found {}", value.len()),
            )),
        )
    })
}

fn conversion(
    index: usize,
    error: impl std::error::Error + Send + Sync + 'static,
) -> rusqlite::Error {
    rusqlite::Error::FromSqlConversionFailure(index, Type::Integer, Box::new(error))
}

#[cfg(test)]
mod tests {
    use shadow_domain::{AssetLocation, Platform, RepresentationKind};

    use super::*;
    use crate::RegisterAsset;

    #[test]
    fn cached_artifact_round_trips_and_replaces_one_variant() {
        let (mut catalog, representation_id, source) = registered_catalog();
        for (version, digest_byte) in [("1", 1_u8), ("2", 2_u8)] {
            let status = catalog
                .record_cached_artifact(&RecordCachedArtifact {
                    representation_id,
                    expected_source: source,
                    artifact: artifact(version, digest_byte),
                })
                .expect("record artifact");
            assert_eq!(status, RecordCachedArtifactStatus::Recorded);
        }

        let records = catalog
            .cached_artifacts(representation_id)
            .expect("read artifacts");
        assert_eq!(records.len(), 1);
        assert_eq!(records[0].source, source);
        assert_eq!(records[0].artifact.generator_version, "2");
        assert_eq!(records[0].artifact.blob_digest, [2_u8; 32]);
    }

    #[test]
    fn stale_artifact_reference_is_not_committed() {
        let (mut catalog, representation_id, source) = registered_catalog();
        let status = catalog
            .record_cached_artifact(&RecordCachedArtifact {
                representation_id,
                expected_source: RepresentationFingerprint {
                    byte_len: source.byte_len + 1,
                    modified_at_ms: source.modified_at_ms,
                },
                artifact: artifact("1", 1),
            })
            .expect("reject stale artifact");

        assert_eq!(status, RecordCachedArtifactStatus::StaleSource);
        assert!(
            catalog
                .cached_artifacts(representation_id)
                .expect("read artifacts")
                .is_empty()
        );
    }

    #[test]
    fn recipe_preview_is_ignored_until_a_working_recipe_names_its_digest() {
        let (mut catalog, representation_id, source) = registered_catalog();
        let source_preview = artifact("1", 1);
        catalog
            .record_cached_artifact(&RecordCachedArtifact {
                representation_id,
                expected_source: source,
                artifact: source_preview,
            })
            .expect("record source preview");

        let recipe_preview = CachedArtifact {
            role: CachedArtifactRole::RecipePreview,
            variant_key: "shadow-recipe-preview:test;recipe=aa".into(),
            generator_id: "shadow-edit-preview".into(),
            generator_version: "test".into(),
            recipe_snapshot_digest: Some([0xaa; 32]),
            provider_preview_id: None,
            blob_algorithm: "blake3-256".into(),
            blob_digest: [2; 32],
            blob_byte_len: 2_048,
            codec: PreviewCodec::Jpeg,
            byte_order: PreviewByteOrder::NotApplicable,
            dimensions: ImageDimensions {
                width: 2_048,
                height: 1_365,
            },
            bits_per_channel: 8,
            channels: 3,
            created_at_ms: 200,
        };
        catalog
            .record_cached_artifact(&RecordCachedArtifact {
                representation_id,
                expected_source: source,
                artifact: recipe_preview,
            })
            .expect("record unattached Recipe preview");

        assert_eq!(
            catalog
                .preferred_cached_artifact(representation_id)
                .expect("select preferred preview")
                .expect("source preview remains available")
                .artifact
                .role,
            CachedArtifactRole::EmbeddedPreview
        );
    }

    #[test]
    fn invalidation_cannot_delete_a_concurrently_replaced_artifact() {
        let (mut catalog, representation_id, source) = registered_catalog();
        catalog
            .record_cached_artifact(&RecordCachedArtifact {
                representation_id,
                expected_source: source,
                artifact: artifact("1", 1),
            })
            .expect("record first artifact");
        let stale = catalog
            .cached_artifacts(representation_id)
            .expect("read first artifact")
            .remove(0);

        let mut replacement = artifact("2", 2);
        replacement.created_at_ms += 1;
        catalog
            .record_cached_artifact(&RecordCachedArtifact {
                representation_id,
                expected_source: source,
                artifact: replacement,
            })
            .expect("record replacement");
        assert_eq!(
            catalog
                .invalidate_cached_artifact(&stale)
                .expect("ignore stale invalidation"),
            InvalidateCachedArtifactStatus::NotCurrent
        );

        let current = catalog
            .cached_artifacts(representation_id)
            .expect("read replacement")
            .remove(0);
        assert_eq!(current.artifact.generator_version, "2");
        assert_eq!(
            catalog
                .invalidate_cached_artifact(&current)
                .expect("invalidate current artifact"),
            InvalidateCachedArtifactStatus::Invalidated
        );
        assert!(
            catalog
                .cached_artifacts(representation_id)
                .expect("read empty artifacts")
                .is_empty()
        );
    }

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
        (catalog, registered.representation_id, source)
    }

    fn artifact(version: &str, digest_byte: u8) -> CachedArtifact {
        CachedArtifact {
            role: CachedArtifactRole::EmbeddedPreview,
            variant_key: "libraw".into(),
            generator_id: "libraw".into(),
            generator_version: version.into(),
            recipe_snapshot_digest: None,
            provider_preview_id: Some(7),
            blob_algorithm: "blake3-256".into(),
            blob_digest: [digest_byte; 32],
            blob_byte_len: 1_024,
            codec: PreviewCodec::Jpeg,
            byte_order: PreviewByteOrder::NotApplicable,
            dimensions: ImageDimensions {
                width: 1_600,
                height: 1_200,
            },
            bits_per_channel: 8,
            channels: 3,
            created_at_ms: 456,
        }
    }
}
