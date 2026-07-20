use rusqlite::{params, types::Type};
use shadow_domain::{EntityId, ImageDimensions, PreviewByteOrder, PreviewCodec, RepresentationId};

use crate::{
    Catalog, CatalogError, RepresentationFingerprint,
    decode_snapshot::representation_fingerprint_in_transaction, read_id,
};

#[derive(Debug, Copy, Clone, Eq, PartialEq, Hash)]
pub enum CachedArtifactRole {
    EmbeddedPreview,
    GeneratedProxy,
}

impl CachedArtifactRole {
    pub const fn as_str(self) -> &'static str {
        match self {
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
                 provider_preview_id, source_byte_len, source_modified_at_ms, blob_algorithm,
                 blob_digest, blob_byte_len, codec, byte_order, width, height,
                 bits_per_channel, channels, created_at_ms
             ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13,
                       ?14, ?15, ?16, ?17, ?18)
             ON CONFLICT(representation_id, role, variant_key) DO UPDATE SET
                 generator_id = excluded.generator_id,
                 generator_version = excluded.generator_version,
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
                    provider_preview_id, source_byte_len, source_modified_at_ms, blob_algorithm,
                    blob_digest, blob_byte_len, codec, byte_order, width, height,
                    bits_per_channel, channels, created_at_ms
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
                optional_usize(row.get::<_, Option<i64>>(5)?, 5)?,
                non_negative_u64(row.get(6)?, 6)?,
                row.get::<_, Option<i64>>(7)?,
                row.get::<_, String>(8)?,
                digest(row.get(9)?, 9)?,
                non_negative_u64(row.get(10)?, 10)?,
                row.get::<_, String>(11)?,
                row.get::<_, String>(12)?,
                non_negative_u32(row.get(13)?, 13)?,
                non_negative_u32(row.get(14)?, 14)?,
                non_negative_u16(row.get(15)?, 15)?,
                non_negative_u16(row.get(16)?, 16)?,
                row.get::<_, i64>(17)?,
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
    Ok(())
}

fn parse_role(value: String) -> Result<CachedArtifactRole, CatalogError> {
    match value.as_str() {
        "embedded_preview" => Ok(CachedArtifactRole::EmbeddedPreview),
        "generated_proxy" => Ok(CachedArtifactRole::GeneratedProxy),
        _ => Err(unknown("role", value)),
    }
}

fn parse_codec(value: String) -> Result<PreviewCodec, CatalogError> {
    match value.as_str() {
        "unknown" => Ok(PreviewCodec::Unknown),
        "jpeg" => Ok(PreviewCodec::Jpeg),
        "bitmap" => Ok(PreviewCodec::Bitmap),
        "jpeg_xl" => Ok(PreviewCodec::JpegXl),
        "h265" => Ok(PreviewCodec::H265),
        _ => Err(unknown("codec", value)),
    }
}

fn parse_byte_order(value: String) -> Result<PreviewByteOrder, CatalogError> {
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

fn non_negative_u64(value: i64, index: usize) -> rusqlite::Result<u64> {
    u64::try_from(value).map_err(|error| conversion(index, error))
}

fn non_negative_u32(value: i64, index: usize) -> rusqlite::Result<u32> {
    u32::try_from(value).map_err(|error| conversion(index, error))
}

fn non_negative_u16(value: i64, index: usize) -> rusqlite::Result<u16> {
    u16::try_from(value).map_err(|error| conversion(index, error))
}

fn optional_usize(value: Option<i64>, index: usize) -> rusqlite::Result<Option<usize>> {
    value
        .map(|value| usize::try_from(value).map_err(|error| conversion(index, error)))
        .transpose()
}

fn digest(value: Vec<u8>, index: usize) -> rusqlite::Result<[u8; 32]> {
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
