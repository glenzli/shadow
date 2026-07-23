use rusqlite::{OptionalExtension, Transaction, params};
use shadow_ai::{
    DISPLAY_LUMA_CONTRACT_VERSION, TECHNICAL_QUALITY_IMPLEMENTATION_VERSION,
    TECHNICAL_QUALITY_SCHEMA_VERSION, TechnicalQualityObservation,
};
use shadow_domain::{EntityId, RepresentationId};

use crate::{
    CachedArtifact, Catalog, CatalogError, RepresentationFingerprint, cache_artifact::digest,
    decode_snapshot::representation_fingerprint_in_transaction,
};

const MAX_REVISION_BYTES: usize = 256;

/// Exact technical-observation contract requested by a reader.
///
/// Callers should normally construct this with [`Self::current`]. Keeping every
/// component explicit prevents an old preprocessing or implementation result
/// from becoming current merely because it finished later.
#[derive(Debug, Clone, Eq, PartialEq, Hash)]
pub struct TechnicalObservationRevision {
    pub observation_schema: u32,
    pub implementation_version: String,
    pub display_luma_contract_version: u32,
    pub preprocessing_version: String,
}

impl TechnicalObservationRevision {
    pub fn current(preprocessing_version: impl Into<String>) -> Self {
        Self {
            observation_schema: TECHNICAL_QUALITY_SCHEMA_VERSION,
            implementation_version: TECHNICAL_QUALITY_IMPLEMENTATION_VERSION.into(),
            display_luma_contract_version: DISPLAY_LUMA_CONTRACT_VERSION,
            preprocessing_version: preprocessing_version.into(),
        }
    }

    pub fn is_supported_by_this_build(&self) -> bool {
        self.observation_schema == TECHNICAL_QUALITY_SCHEMA_VERSION
            && self.implementation_version == TECHNICAL_QUALITY_IMPLEMENTATION_VERSION
            && self.display_luma_contract_version == DISPLAY_LUMA_CONTRACT_VERSION
            && valid_revision_text(&self.preprocessing_version)
    }
}

#[derive(Debug, Clone, PartialEq)]
pub struct RecordTechnicalObservation {
    pub representation_id: RepresentationId,
    pub expected_source: RepresentationFingerprint,
    pub expected_artifact: CachedArtifact,
    pub observation: TechnicalQualityObservation,
    pub observed_at_ms: i64,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub enum RecordTechnicalObservationStatus {
    Recorded,
    StaleInput,
}

#[derive(Debug, Clone, PartialEq)]
pub struct TechnicalObservationRecord {
    pub representation_id: RepresentationId,
    pub source: RepresentationFingerprint,
    pub source_artifact: CachedArtifact,
    pub observation: TechnicalQualityObservation,
    pub observation_digest: [u8; 32],
    pub observed_at_ms: i64,
}

/// Compact Review-grid projection. The full 256-bin histogram remains in the
/// typed persisted observation and is deliberately not copied into every item.
#[derive(Debug, Clone, PartialEq)]
pub struct TechnicalObservationSummary {
    pub input_width: u32,
    pub input_height: u32,
    pub preprocessing_version: String,
    pub implementation_version: String,
    pub mean_luma: f64,
    pub p01_luma: f64,
    pub p50_luma: f64,
    pub p99_luma: f64,
    pub near_black_fraction: f64,
    pub near_white_fraction: f64,
    pub laplacian_variance: f64,
    pub edge_energy: f64,
}

impl From<&TechnicalQualityObservation> for TechnicalObservationSummary {
    fn from(observation: &TechnicalQualityObservation) -> Self {
        let metrics = &observation.metrics;
        Self {
            input_width: observation.input.width,
            input_height: observation.input.height,
            preprocessing_version: observation.input.preprocessing_version.clone(),
            implementation_version: observation.algorithm.implementation_version().into(),
            mean_luma: metrics.mean_luma.get(),
            p01_luma: metrics.p01_luma.get(),
            p50_luma: metrics.p50_luma.get(),
            p99_luma: metrics.p99_luma.get(),
            near_black_fraction: metrics.near_black_fraction.get(),
            near_white_fraction: metrics.near_white_fraction.get(),
            laplacian_variance: metrics.laplacian_variance.get(),
            edge_energy: metrics.edge_energy.get(),
        }
    }
}

impl Catalog {
    /// Records one deterministic display-luma observation if its exact source
    /// representation and cached-artifact slot are still current.
    ///
    /// The natural key includes every algorithm/input revision and the complete
    /// artifact identity. Repeating the same request is an idempotent upsert;
    /// an older implementation or preprocessing result occupies a different
    /// key and therefore cannot overwrite a newer result.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] for malformed provenance, serialization or
    /// integer conversion failures, and `SQLite` errors. Normal source races are
    /// returned as [`RecordTechnicalObservationStatus::StaleInput`].
    pub fn record_technical_observation(
        &mut self,
        request: &RecordTechnicalObservation,
    ) -> Result<RecordTechnicalObservationStatus, CatalogError> {
        let revision = validate_observation(request)?;
        let observation_json = serialize_observation(&request.observation)?;
        let observation_digest = blake3::hash(observation_json.as_bytes());
        let transaction = self.connection.transaction()?;
        let current_source =
            representation_fingerprint_in_transaction(&transaction, request.representation_id)?;
        if current_source != request.expected_source
            || !artifact_is_current(
                &transaction,
                request.representation_id,
                request.expected_source,
                &request.expected_artifact,
            )?
        {
            return Ok(RecordTechnicalObservationStatus::StaleInput);
        }

        let artifact = &request.expected_artifact;
        transaction.execute(
            "INSERT INTO representation_technical_observations(
                 representation_id, source_role, source_variant_key, source_generator_id,
                 source_generator_version, source_provider_preview_id, source_blob_algorithm,
                 source_blob_digest, source_blob_byte_len, source_codec, source_byte_order,
                 source_width, source_height, source_bits_per_channel, source_channels,
                 source_created_at_ms, source_byte_len, source_modified_at_ms,
                 observation_schema, implementation_version, display_luma_contract_version,
                 preprocessing_version, observation_json, observation_digest, observed_at_ms
             ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13,
                       ?14, ?15, ?16, ?17, ?18, ?19, ?20, ?21, ?22, ?23, ?24, ?25)
             ON CONFLICT(
                 representation_id, source_role, source_variant_key, source_generator_id,
                 source_generator_version, source_provider_preview_id, source_blob_algorithm,
                 source_blob_digest, source_blob_byte_len, source_codec, source_byte_order,
                 source_width, source_height, source_bits_per_channel, source_channels,
                 observation_schema,
                 implementation_version, display_luma_contract_version, preprocessing_version
             ) DO UPDATE SET
                 source_created_at_ms = excluded.source_created_at_ms,
                 source_byte_len = excluded.source_byte_len,
                 source_modified_at_ms = excluded.source_modified_at_ms,
                 observation_json = excluded.observation_json,
                 observation_digest = excluded.observation_digest,
                 observed_at_ms = excluded.observed_at_ms",
            params![
                request.representation_id.as_bytes().as_slice(),
                artifact.role.as_str(),
                artifact.variant_key,
                artifact.generator_id,
                artifact.generator_version,
                provider_preview_key(artifact.provider_preview_id)?,
                artifact.blob_algorithm,
                artifact.blob_digest.as_slice(),
                sqlite_u64(artifact.blob_byte_len, "source_blob_byte_len")?,
                artifact.codec.as_str(),
                artifact.byte_order.as_str(),
                i64::from(artifact.dimensions.width),
                i64::from(artifact.dimensions.height),
                i64::from(artifact.bits_per_channel),
                i64::from(artifact.channels),
                artifact.created_at_ms,
                sqlite_u64(request.expected_source.byte_len, "source_byte_len")?,
                request.expected_source.modified_at_ms,
                i64::from(revision.observation_schema),
                revision.implementation_version,
                i64::from(revision.display_luma_contract_version),
                revision.preprocessing_version,
                observation_json,
                observation_digest.as_bytes().as_slice(),
                request.observed_at_ms,
            ],
        )?;
        transaction.commit()?;
        Ok(RecordTechnicalObservationStatus::Recorded)
    }

    /// Reads one observation only when its exact source artifact remains current
    /// and the requested contract is supported by this build.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when a matching persisted row is malformed or
    /// fails its content-integrity checks.
    pub fn technical_observation(
        &self,
        representation_id: RepresentationId,
        expected_source: RepresentationFingerprint,
        expected_artifact: &CachedArtifact,
        revision: &TechnicalObservationRevision,
    ) -> Result<Option<TechnicalObservationRecord>, CatalogError> {
        if !revision.is_supported_by_this_build() {
            return Ok(None);
        }
        let row = self
            .connection
            .query_row(
                "SELECT o.observation_json, o.observation_digest, o.observed_at_ms
                 FROM representation_technical_observations o
                 JOIN representations r ON r.id = o.representation_id
                 JOIN representation_cached_artifacts a
                   ON a.representation_id = o.representation_id
                  AND a.role = o.source_role
                  AND a.variant_key = o.source_variant_key
                  AND a.generator_id = o.source_generator_id
                  AND a.generator_version = o.source_generator_version
                  AND COALESCE(a.provider_preview_id, -1) = o.source_provider_preview_id
                  AND a.blob_algorithm = o.source_blob_algorithm
                  AND a.blob_digest = o.source_blob_digest
                  AND a.blob_byte_len = o.source_blob_byte_len
                  AND a.codec = o.source_codec
                  AND a.byte_order = o.source_byte_order
                  AND a.width = o.source_width AND a.height = o.source_height
                  AND a.bits_per_channel = o.source_bits_per_channel
                  AND a.channels = o.source_channels
                  AND a.created_at_ms = o.source_created_at_ms
                  AND a.source_byte_len = o.source_byte_len
                  AND a.source_modified_at_ms IS o.source_modified_at_ms
                 WHERE o.representation_id = ?1
                   AND r.byte_len = ?2 AND r.modified_at_ms IS ?3
                   AND o.source_byte_len = ?2 AND o.source_modified_at_ms IS ?3
                   AND o.source_role = ?4 AND o.source_variant_key = ?5
                   AND o.source_generator_id = ?6 AND o.source_generator_version = ?7
                   AND o.source_provider_preview_id = ?8
                   AND o.source_blob_algorithm = ?9 AND o.source_blob_digest = ?10
                   AND o.source_blob_byte_len = ?11
                   AND o.source_codec = ?12 AND o.source_byte_order = ?13
                   AND o.source_width = ?14 AND o.source_height = ?15
                   AND o.source_bits_per_channel = ?16 AND o.source_channels = ?17
                   AND o.source_created_at_ms = ?18
                   AND o.observation_schema = ?19 AND o.implementation_version = ?20
                   AND o.display_luma_contract_version = ?21
                   AND o.preprocessing_version = ?22",
                params![
                    representation_id.as_bytes().as_slice(),
                    sqlite_u64(expected_source.byte_len, "source_byte_len")?,
                    expected_source.modified_at_ms,
                    expected_artifact.role.as_str(),
                    expected_artifact.variant_key,
                    expected_artifact.generator_id,
                    expected_artifact.generator_version,
                    provider_preview_key(expected_artifact.provider_preview_id)?,
                    expected_artifact.blob_algorithm,
                    expected_artifact.blob_digest.as_slice(),
                    sqlite_u64(expected_artifact.blob_byte_len, "source_blob_byte_len")?,
                    expected_artifact.codec.as_str(),
                    expected_artifact.byte_order.as_str(),
                    i64::from(expected_artifact.dimensions.width),
                    i64::from(expected_artifact.dimensions.height),
                    i64::from(expected_artifact.bits_per_channel),
                    i64::from(expected_artifact.channels),
                    expected_artifact.created_at_ms,
                    i64::from(revision.observation_schema),
                    revision.implementation_version,
                    i64::from(revision.display_luma_contract_version),
                    revision.preprocessing_version,
                ],
                |row| {
                    Ok((
                        row.get::<_, String>(0)?,
                        digest(row.get(1)?, 1)?,
                        row.get::<_, i64>(2)?,
                    ))
                },
            )
            .optional()?;
        row.map(|(json, stored_digest, observed_at_ms)| {
            let observation =
                decode_observation(&json, stored_digest, revision, expected_artifact)?;
            Ok(TechnicalObservationRecord {
                representation_id,
                source: expected_source,
                source_artifact: expected_artifact.clone(),
                observation,
                observation_digest: stored_digest,
                observed_at_ms,
            })
        })
        .transpose()
    }
}

pub(crate) fn decode_observation(
    json: &str,
    stored_digest: [u8; 32],
    revision: &TechnicalObservationRevision,
    source_artifact: &CachedArtifact,
) -> Result<TechnicalQualityObservation, CatalogError> {
    decode_observation_with_source_hash(
        json,
        stored_digest,
        revision,
        &artifact_content_hash(source_artifact),
    )
}

pub(crate) fn decode_observation_with_source_hash(
    json: &str,
    stored_digest: [u8; 32],
    revision: &TechnicalObservationRevision,
    source_hash: &str,
) -> Result<TechnicalQualityObservation, CatalogError> {
    if blake3::hash(json.as_bytes()).as_bytes() != &stored_digest {
        return Err(CatalogError::InvalidPersistedTechnicalObservation(
            "observation JSON digest does not match",
        ));
    }
    let observation: TechnicalQualityObservation =
        serde_json::from_str(json).map_err(CatalogError::TechnicalObservationJson)?;
    if observation.algorithm.schema_version() != revision.observation_schema
        || observation.algorithm.implementation_version() != revision.implementation_version
        || observation.input.display_luma_contract_version != revision.display_luma_contract_version
        || observation.input.preprocessing_version != revision.preprocessing_version
        || observation.input.input_source_hash != source_hash
    {
        return Err(CatalogError::InvalidPersistedTechnicalObservation(
            "observation JSON provenance disagrees with indexed columns",
        ));
    }
    Ok(observation)
}

pub(crate) fn artifact_content_hash(artifact: &CachedArtifact) -> String {
    artifact_content_hash_parts(&artifact.blob_algorithm, artifact.blob_digest)
}

pub(crate) fn artifact_content_hash_parts(algorithm: &str, digest: [u8; 32]) -> String {
    format!(
        "{}:{}",
        algorithm,
        blake3::Hash::from_bytes(digest).to_hex()
    )
}

fn validate_observation(
    request: &RecordTechnicalObservation,
) -> Result<TechnicalObservationRevision, CatalogError> {
    let artifact = &request.expected_artifact;
    if artifact.variant_key.is_empty()
        || artifact.generator_id.is_empty()
        || artifact.blob_algorithm.is_empty()
        || artifact.blob_byte_len == 0
    {
        return Err(CatalogError::InvalidTechnicalObservation(
            "source artifact identity is incomplete".into(),
        ));
    }
    let observation = &request.observation;
    let revision = TechnicalObservationRevision {
        observation_schema: observation.algorithm.schema_version(),
        implementation_version: observation.algorithm.implementation_version().into(),
        display_luma_contract_version: observation.input.display_luma_contract_version,
        preprocessing_version: observation.input.preprocessing_version.clone(),
    };
    if !revision.is_supported_by_this_build() {
        return Err(CatalogError::InvalidTechnicalObservation(
            "observation revision is not supported by this build".into(),
        ));
    }
    if observation.input.input_source_hash != artifact_content_hash(artifact) {
        return Err(CatalogError::InvalidTechnicalObservation(
            "input source hash does not match the cached artifact digest".into(),
        ));
    }
    Ok(revision)
}

fn artifact_is_current(
    transaction: &Transaction<'_>,
    representation_id: RepresentationId,
    source: RepresentationFingerprint,
    artifact: &CachedArtifact,
) -> Result<bool, CatalogError> {
    let present = transaction
        .query_row(
            "SELECT 1 FROM representation_cached_artifacts
             WHERE representation_id = ?1 AND role = ?2 AND variant_key = ?3
               AND generator_id = ?4 AND generator_version = ?5
               AND provider_preview_id IS ?6
               AND source_byte_len = ?7 AND source_modified_at_ms IS ?8
               AND blob_algorithm = ?9 AND blob_digest = ?10 AND blob_byte_len = ?11
               AND codec = ?12 AND byte_order = ?13 AND width = ?14 AND height = ?15
               AND bits_per_channel = ?16 AND channels = ?17 AND created_at_ms = ?18",
            params![
                representation_id.as_bytes().as_slice(),
                artifact.role.as_str(),
                artifact.variant_key,
                artifact.generator_id,
                artifact.generator_version,
                provider_preview_value(artifact.provider_preview_id)?,
                sqlite_u64(source.byte_len, "source_byte_len")?,
                source.modified_at_ms,
                artifact.blob_algorithm,
                artifact.blob_digest.as_slice(),
                sqlite_u64(artifact.blob_byte_len, "source_blob_byte_len")?,
                artifact.codec.as_str(),
                artifact.byte_order.as_str(),
                i64::from(artifact.dimensions.width),
                i64::from(artifact.dimensions.height),
                i64::from(artifact.bits_per_channel),
                i64::from(artifact.channels),
                artifact.created_at_ms,
            ],
            |_| Ok(()),
        )
        .optional()?
        .is_some();
    Ok(present)
}

fn serialize_observation(
    observation: &TechnicalQualityObservation,
) -> Result<String, CatalogError> {
    serde_json::to_string(observation).map_err(CatalogError::TechnicalObservationJson)
}

fn valid_revision_text(value: &str) -> bool {
    !value.trim().is_empty() && value.len() <= MAX_REVISION_BYTES
}

fn provider_preview_key(value: Option<usize>) -> Result<i64, CatalogError> {
    value.map_or(Ok(-1), |value| {
        i64::try_from(value).map_err(|_| CatalogError::TechnicalObservationValueOutOfRange {
            field: "source_provider_preview_id",
        })
    })
}

fn provider_preview_value(value: Option<usize>) -> Result<Option<i64>, CatalogError> {
    value
        .map(|value| {
            i64::try_from(value).map_err(|_| CatalogError::TechnicalObservationValueOutOfRange {
                field: "source_provider_preview_id",
            })
        })
        .transpose()
}

fn sqlite_u64(value: u64, field: &'static str) -> Result<i64, CatalogError> {
    i64::try_from(value).map_err(|_| CatalogError::TechnicalObservationValueOutOfRange { field })
}

#[cfg(test)]
mod tests {
    use shadow_ai::{
        DISPLAY_LUMA_CONTRACT_VERSION, DisplayLumaPlane, NonNegativeFinite, observe_display_luma,
    };
    use shadow_domain::{
        AssetLocation, ImageDimensions, Platform, PreviewByteOrder, PreviewCodec,
        RepresentationKind,
    };

    use super::*;
    use crate::{CachedArtifactRole, RecordCachedArtifact, RegisterAsset};

    #[test]
    fn exact_observation_round_trips_and_idempotently_updates_one_identity() {
        let (mut catalog, representation_id, source, artifact) = fixture();
        let first = request(representation_id, source, &artifact, "jpeg-luma-v1", 200);
        assert_eq!(
            catalog
                .record_technical_observation(&first)
                .expect("record first observation"),
            RecordTechnicalObservationStatus::Recorded
        );
        let mut repeated = first.clone();
        repeated.observed_at_ms = 201;
        assert_eq!(
            catalog
                .record_technical_observation(&repeated)
                .expect("repeat observation"),
            RecordTechnicalObservationStatus::Recorded
        );
        let revision = TechnicalObservationRevision::current("jpeg-luma-v1");
        let record = catalog
            .technical_observation(representation_id, source, &artifact, &revision)
            .expect("read observation")
            .expect("current observation");
        assert_eq!(record.observed_at_ms, 201);
        assert_eq!(record.observation, repeated.observation);
        let count: i64 = catalog
            .connection
            .query_row(
                "SELECT COUNT(*) FROM representation_technical_observations",
                [],
                |row| row.get(0),
            )
            .expect("count observations");
        assert_eq!(count, 1);
    }

    #[test]
    fn persisted_float_integrity_does_not_require_textual_reserialization_stability() {
        let (mut catalog, representation_id, source, artifact) = fixture();
        let mut request = request(representation_id, source, &artifact, "jpeg-luma-v1", 200);
        request.observation.metrics.edge_energy =
            NonNegativeFinite::new(0.000_371_511_986_703_282_44).expect("valid real metric");
        catalog
            .record_technical_observation(&request)
            .expect("record real floating-point observation");

        let record = catalog
            .technical_observation(
                representation_id,
                source,
                &artifact,
                &TechnicalObservationRevision::current("jpeg-luma-v1"),
            )
            .expect("read digest-valid typed JSON")
            .expect("observation exists");
        assert!(
            (record.observation.metrics.edge_energy.get()
                - request.observation.metrics.edge_energy.get())
            .abs()
                < 1.0e-18
        );
    }

    #[test]
    fn stale_source_or_replaced_exact_artifact_is_not_recorded() {
        let (mut catalog, representation_id, source, artifact) = fixture();
        let stale_source = RecordTechnicalObservation {
            expected_source: RepresentationFingerprint {
                byte_len: source.byte_len + 1,
                modified_at_ms: source.modified_at_ms,
            },
            ..request(representation_id, source, &artifact, "jpeg-luma-v1", 200)
        };
        assert_eq!(
            catalog
                .record_technical_observation(&stale_source)
                .expect("reject stale source"),
            RecordTechnicalObservationStatus::StaleInput
        );

        let mut replacement = artifact.clone();
        replacement.generator_version = "2".into();
        replacement.created_at_ms += 1;
        catalog
            .record_cached_artifact(&RecordCachedArtifact {
                representation_id,
                expected_source: source,
                artifact: replacement,
            })
            .expect("replace visual");
        assert_eq!(
            catalog
                .record_technical_observation(&request(
                    representation_id,
                    source,
                    &artifact,
                    "jpeg-luma-v1",
                    201,
                ))
                .expect("reject replaced artifact"),
            RecordTechnicalObservationStatus::StaleInput
        );
    }

    #[test]
    fn revisions_coexist_and_corrupt_current_payload_fails_integrity_check() {
        let (mut catalog, representation_id, source, artifact) = fixture();
        for (preprocessing, observed_at_ms) in [("jpeg-luma-v1", 200), ("jpeg-luma-v2", 201)] {
            catalog
                .record_technical_observation(&request(
                    representation_id,
                    source,
                    &artifact,
                    preprocessing,
                    observed_at_ms,
                ))
                .expect("record revision");
        }
        for preprocessing in ["jpeg-luma-v1", "jpeg-luma-v2"] {
            assert!(
                catalog
                    .technical_observation(
                        representation_id,
                        source,
                        &artifact,
                        &TechnicalObservationRevision::current(preprocessing),
                    )
                    .expect("read exact revision")
                    .is_some()
            );
        }
        let unsupported = TechnicalObservationRevision {
            observation_schema: TECHNICAL_QUALITY_SCHEMA_VERSION + 1,
            ..TechnicalObservationRevision::current("jpeg-luma-v2")
        };
        assert!(
            catalog
                .technical_observation(representation_id, source, &artifact, &unsupported)
                .expect("unknown revisions are absent")
                .is_none()
        );

        catalog
            .connection
            .execute(
                "UPDATE representation_technical_observations
                 SET observation_digest = zeroblob(32)
                 WHERE preprocessing_version = 'jpeg-luma-v2'",
                [],
            )
            .expect("corrupt digest fixture");
        assert!(matches!(
            catalog.technical_observation(
                representation_id,
                source,
                &artifact,
                &TechnicalObservationRevision::current("jpeg-luma-v2"),
            ),
            Err(CatalogError::InvalidPersistedTechnicalObservation(
                "observation JSON digest does not match"
            ))
        ));
    }

    fn fixture() -> (
        Catalog,
        RepresentationId,
        RepresentationFingerprint,
        CachedArtifact,
    ) {
        let mut catalog = Catalog::open_in_memory().expect("open catalog");
        let source = RepresentationFingerprint {
            byte_len: 4_096,
            modified_at_ms: Some(123),
        };
        let registered = catalog
            .register_asset(&RegisterAsset {
                kind: RepresentationKind::OriginalRaw,
                location: AssetLocation::new(
                    Platform::MacOs,
                    b"/photos/technical.dng".to_vec(),
                    "/photos/technical.dng",
                ),
                byte_len: source.byte_len,
                modified_at_ms: source.modified_at_ms,
                now_ms: 100,
            })
            .expect("register source");
        let artifact = CachedArtifact {
            role: CachedArtifactRole::GeneratedProxy,
            variant_key: "libraw:grid-jpeg-2048-q88-444-v3".into(),
            generator_id: "libraw".into(),
            generator_version: "1".into(),
            recipe_snapshot_digest: None,
            provider_preview_id: None,
            blob_algorithm: "blake3-256".into(),
            blob_digest: [7; 32],
            blob_byte_len: 1_024,
            codec: PreviewCodec::Jpeg,
            byte_order: PreviewByteOrder::NotApplicable,
            dimensions: ImageDimensions {
                width: 2_048,
                height: 1_365,
            },
            bits_per_channel: 8,
            channels: 3,
            created_at_ms: 150,
        };
        catalog
            .record_cached_artifact(&RecordCachedArtifact {
                representation_id: registered.representation_id,
                expected_source: source,
                artifact: artifact.clone(),
            })
            .expect("record visual");
        (catalog, registered.representation_id, source, artifact)
    }

    fn request(
        representation_id: RepresentationId,
        source: RepresentationFingerprint,
        artifact: &CachedArtifact,
        preprocessing_version: &str,
        observed_at_ms: i64,
    ) -> RecordTechnicalObservation {
        let input_source_hash = artifact_content_hash(artifact);
        let samples = [0.0, 0.25, 0.75, 1.0];
        let observation = observe_display_luma(DisplayLumaPlane {
            contract_version: DISPLAY_LUMA_CONTRACT_VERSION,
            width: 2,
            height: 2,
            stride: 2,
            samples: &samples,
            preprocessing_version,
            input_source_hash: &input_source_hash,
        })
        .expect("observe luma");
        RecordTechnicalObservation {
            representation_id,
            expected_source: source,
            expected_artifact: artifact.clone(),
            observation,
            observed_at_ms,
        }
    }
}
