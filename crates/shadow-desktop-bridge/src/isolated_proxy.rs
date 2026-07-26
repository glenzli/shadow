//! Crash-isolated generated-preview rendering for the desktop catalog.
//!
//! Embedded camera previews never enter this path. It exists for files that
//! need a native RAW/raster reference render and may therefore cross a third
//! party decoder or a user's private provider module. A provider can return a
//! normal error, but it can also crash the process; the latter must degrade one
//! preview rather than terminate Shadow.

use std::{
    collections::HashMap,
    env, fs,
    io::{self, Read, Seek, SeekFrom},
    path::{Path, PathBuf},
    process::{Child, Command, ExitStatus, Stdio},
    sync::{Mutex, OnceLock, mpsc},
    thread,
    time::{Duration, Instant},
};

use anyhow::{Context, Result, bail};
use shadow_core::fingerprint_source;
use shadow_domain::{
    DecodeCapabilitySnapshot, DecodeProviderSnapshot, DecodeSupport, DecoderSnapshot,
    ImageDimensions, ImageMargins, PendingCorrectionsSnapshot, PreviewCodec,
    PreviewDescriptorSnapshot, ProxyPayload, RawDevelopmentCapabilitySnapshot, RawMetadataSnapshot,
};
use uuid::Uuid;

const HELPER_PATH_ENVIRONMENT: &str = "SHADOW_DECODE_HELPER_PATH";
const DISABLE_PRIVATE_DECODER_ENVIRONMENT: &str = "SHADOW_DISABLE_PRIVATE_DECODER";
const PRIVATE_DECODER_PLUGIN_PATH_ENVIRONMENT: &str = "SHADOW_PRIVATE_DECODER_PLUGIN_PATH";
const RAW_PIPELINE_ENVIRONMENT: &str = "SHADOW_RAW_PIPELINE";
const IMAGE_ACCELERATION_ENVIRONMENT: &str = "SHADOW_IMAGE_ACCELERATION";
const CAMERA_PROFILE_DIRECTORY_ENVIRONMENT: &str = "SHADOW_CAMERA_PROFILE_DIRECTORY";
const HELPER_PROTOCOL: &str = "shadow-proxy-v1";
const METADATA_PROBE_PROTOCOL: &str = "shadow-probe-v1";
const PREVIEW_DEVELOPMENT_PROBE_PROTOCOL: &str = "shadow-probe-v2";
const METADATA_SNAPSHOT_PROTOCOL: &str = "shadow-metadata-v2";
const DECODER_SNAPSHOT_PROTOCOL: &str = "shadow-inspect-v3";
const MAX_PROXY_BYTES: u64 = 64 * 1024 * 1024;
const MAX_HELPER_STREAM_BYTES: usize = 64 * 1024;
const MAX_RECEIPT_TEXT_BYTES: usize = 16 * 1024;
const MAX_METADATA_TEXT_BYTES: usize = 4 * 1024;
const MAX_DECODER_SNAPSHOT_IDENTITY_TEXT_BYTES: usize = 4 * 1024;
const MAX_DECODER_SNAPSHOT_METADATA_TEXT_BYTES: usize = 1024;
const MAX_DECODER_SNAPSHOT_PREVIEWS: usize = 64;
const HELPER_TIMEOUT: Duration = Duration::from_secs(30);
const HELPER_POLL_INTERVAL: Duration = Duration::from_millis(10);
// A helper may exit while an SDK-created descendant still owns stdout/stderr.
// Never let those inherited descriptors turn a bounded decoder operation into
// an unbounded join in the desktop process.
const HELPER_PIPE_DRAIN_TIMEOUT: Duration = Duration::from_secs(2);
const SOURCE_SIGNATURE_SAMPLE_BYTES: usize = 64 * 1024;
const REFERENCE_PROXY_OPERATION: &str = "reference-proxy";
const METADATA_PROBE_OPERATION: &str = "metadata-probe";
const PREVIEW_DEVELOPMENT_PROBE_OPERATION: &str = "preview-development-probe";
const METADATA_SNAPSHOT_OPERATION: &str = "metadata-snapshot";
const DECODER_SNAPSHOT_OPERATION: &str = "decoder-snapshot";
const SAFETY_RECORD_SCHEMA: u8 = 1;
const PREVIEW_DEVELOPMENT_RECEIPT_SCHEMA: u8 = 1;
const METADATA_SNAPSHOT_SCHEMA: u8 = 1;
const DECODER_SNAPSHOT_SCHEMA: u8 = 1;
const METADATA_SNAPSHOT_FIELD_COUNT: usize = 38;
const RAW_METADATA_SNAPSHOT_FIELD_COUNT: usize = 33;
const DECODER_SNAPSHOT_CAPABILITY_FIELD_COUNT: usize = 16;
const DECODER_SNAPSHOT_BASE_FIELD_COUNT: usize = 58;
const DECODER_SNAPSHOT_PREVIEW_FIELD_COUNT: usize = 8;

#[cfg(unix)]
unsafe extern "C" {
    fn killpg(pgrp: i32, sig: i32) -> i32;
}

/// What the isolated helper established about this specific source route.
///
/// A successful v1 proxy only means that a child process rendered one bounded
/// RGB proxy.  It must never be mistaken for evidence that the host may safely
/// open a RawFrame, a full-detail session, or an export session in-process.
#[derive(Debug, Clone, Copy, Eq, PartialEq, serde::Serialize, serde::Deserialize)]
#[serde(rename_all = "snake_case")]
pub(crate) enum IsolatedDecodeObservation {
    /// The legacy helper completed only its requested bounded stage. Native
    /// RawFrame/detail/export safety is intentionally still unknown.
    LegacyStageSucceededUnproven,
    /// The child did not exit normally (for example SIGSEGV or abort).
    ChildCrashed,
    /// The helper exceeded the bounded child-process deadline and was killed.
    TimedOut,
    /// The helper completed normally but reported a decoder error.
    ChildRejected,
    /// The helper exited successfully but violated its small stdout/JPEG
    /// protocol.
    InvalidOutput,
    /// The helper executable could not be started or observed.
    LaunchFailed,
}

impl IsolatedDecodeObservation {
    fn quarantines_main_process_native_decode(self) -> bool {
        matches!(self, Self::ChildCrashed | Self::TimedOut)
    }

    pub(crate) fn diagnostic_label(self) -> &'static str {
        match self {
            Self::LegacyStageSucceededUnproven => {
                "legacy child stage succeeded (RawFrame unproven)"
            }
            Self::ChildCrashed => "child decoder crashed",
            Self::TimedOut => "child decoder timed out",
            Self::ChildRejected => "child decoder rejected the source",
            Self::InvalidOutput => "child decoder returned invalid output",
            Self::LaunchFailed => "child decoder could not start",
        }
    }
}

/// The only admission decision this v1 safety layer is allowed to make.
///
/// `NotQuarantined` deliberately does *not* mean "native RAW safe". It means
/// only that this exact source/helper/proxy route has not previously crashed or
/// timed out in the isolated child. A later helper protocol can add explicit
/// per-stage RawFrame receipts without changing this conservative meaning.
#[derive(Debug, Clone, Copy, Eq, PartialEq)]
pub(crate) enum NativeDecodeAdmission {
    NotQuarantined,
    Quarantined {
        observation: IsolatedDecodeObservation,
    },
}

/// A bounded child-process receipt for one actual preview RAW-development
/// attempt. This is useful provenance for a later worker route, but is not a
/// license to repeat the same RawFrame work inside the desktop process.
#[derive(Debug, Clone, Eq, PartialEq, serde::Serialize, serde::Deserialize)]
pub(crate) struct IsolatedPreviewDevelopmentReceipt {
    pub(crate) raw_pipeline_path: u8,
    pub(crate) source_provider_id: String,
    pub(crate) source_provider_version: String,
    pub(crate) pipeline_receipt_identity: String,
    pub(crate) requested_plan_identity: String,
    pub(crate) effective_plan_identity: String,
}

/// A child-established metadata snapshot. The router identity identifies the
/// configured helper-side provider graph; it does not claim that the desktop
/// process may reopen the source with that provider.
#[derive(Debug, Clone, PartialEq, serde::Serialize, serde::Deserialize)]
pub(crate) struct IsolatedPhotoMetadataSnapshot {
    pub(crate) router_provider_id: String,
    pub(crate) router_provider_version: String,
    pub(crate) metadata: RawMetadataSnapshot,
}

/// A child-established, descriptor-only decoder inspection. The nested
/// `snapshot` intentionally describes the helper-side router; callers that
/// store it through a host `DecodeInspector` must normalize the cache-facing
/// provider id/version first. The helper can load a private provider which the
/// desktop host deliberately never links.
#[derive(Debug, Clone, PartialEq, serde::Serialize, serde::Deserialize)]
pub(crate) struct IsolatedPhotoDecoderSnapshot {
    pub(crate) router_provider_id: String,
    pub(crate) router_provider_version: String,
    pub(crate) snapshot: DecoderSnapshot,
}

impl IsolatedPhotoDecoderSnapshot {
    /// Produces the catalog-facing snapshot for the desktop router without
    /// pretending that the helper's provider graph is in-process. This is a
    /// **catalog-only** snapshot: metadata survives, but every capability that
    /// would let a caller reopen/decode/develop source pixels in the desktop
    /// process is explicitly unavailable. The generated helper proxy remains
    /// the only safe visual path for this route.
    pub(crate) fn into_catalog_snapshot(
        mut self,
        provider_id: &str,
        provider_version: &str,
    ) -> DecoderSnapshot {
        self.snapshot.provider = DecodeProviderSnapshot {
            id: provider_id.to_owned(),
            version: provider_version.to_owned(),
            // The child established its own provider graph, not an in-process
            // claim about the host router. Do not expose helper implementation
            // flags after normalizing the identity.
            dng_sdk: false,
            rawspeed: false,
            jpeg: false,
        };
        self.snapshot.capabilities = DecodeCapabilitySnapshot {
            // The copied snapshot contains metadata, so catalog projection may
            // safely consume it without reopening the RAW.
            metadata: DecodeSupport::Available,
            embedded_previews: DecodeSupport::Unavailable,
            raw_frame: DecodeSupport::Unavailable,
            reference_rgb: DecodeSupport::Unavailable,
            // DNG opcode presence is source metadata, not a permission for
            // the host to execute those opcodes.
            pending_corrections: self.snapshot.capabilities.pending_corrections,
            raw_development: RawDevelopmentCapabilitySnapshot::default(),
        };
        self.snapshot.previews.clear();
        self.snapshot
    }
}

#[derive(Debug, Clone, Eq, PartialEq, Hash, serde::Serialize, serde::Deserialize)]
struct DecoderSafetyKey {
    source_path: String,
    source_byte_len: u64,
    source_modified_at_ms: Option<i64>,
    /// A bounded content sample prevents a same-length, timestamp-preserving
    /// replacement from reusing a stale safety decision or decoder snapshot.
    /// This is deliberately not a catalog-wide full-file hash: it is only the
    /// helper's local admission/cache identity, and costs a small fixed read.
    source_content_sample: String,
    helper_path: String,
    helper_byte_len: Option<u64>,
    helper_modified_at_ms: Option<i64>,
    /// Helper executables are normally small. Hash their full contents rather
    /// than trusting a size/mtime pair, because this identity controls which
    /// private provider graph produced an isolated observation.
    helper_content_digest: Option<String>,
    /// A digest of the decoder-relevant environment as observed by the child.
    /// It includes the explicit private-plugin override and that file's weak
    /// fingerprint when present, but never persists the private path itself.
    provider_environment_identity: String,
    helper_protocol: String,
    operation: String,
}

impl DecoderSafetyKey {
    fn reference_proxy(source_path: &Path, helper_path: &Path) -> Result<Self> {
        Self::for_operation(
            source_path,
            helper_path,
            HELPER_PROTOCOL,
            REFERENCE_PROXY_OPERATION,
        )
    }

    fn metadata_probe(source_path: &Path, helper_path: &Path) -> Result<Self> {
        Self::for_operation(
            source_path,
            helper_path,
            METADATA_PROBE_PROTOCOL,
            METADATA_PROBE_OPERATION,
        )
    }

    fn metadata_snapshot(source_path: &Path, helper_path: &Path) -> Result<Self> {
        Self::for_operation(
            source_path,
            helper_path,
            METADATA_SNAPSHOT_PROTOCOL,
            METADATA_SNAPSHOT_OPERATION,
        )
    }

    fn decoder_snapshot(source_path: &Path, helper_path: &Path) -> Result<Self> {
        Self::for_operation(
            source_path,
            helper_path,
            DECODER_SNAPSHOT_PROTOCOL,
            DECODER_SNAPSHOT_OPERATION,
        )
    }

    fn preview_development(source_path: &Path, helper_path: &Path, max_edge: u32) -> Result<Self> {
        let operation = format!("{PREVIEW_DEVELOPMENT_PROBE_OPERATION}@{max_edge}");
        Self::for_operation(
            source_path,
            helper_path,
            PREVIEW_DEVELOPMENT_PROBE_PROTOCOL,
            &operation,
        )
    }

    fn for_operation(
        source_path: &Path,
        helper_path: &Path,
        helper_protocol: &str,
        operation: &str,
    ) -> Result<Self> {
        let source = fingerprint_source(source_path).with_context(|| {
            format!(
                "fingerprint isolated decoder source {}",
                source_path.display()
            )
        })?;
        // Failure to stat a helper still belongs to the normal launch-error
        // path. The location remains part of the safety key so a replacement
        // executable receives a separate observation even before it can be
        // fingerprinted successfully.
        let helper = fingerprint_source(helper_path).ok();
        Ok(Self {
            source_path: source_path.to_string_lossy().into_owned(),
            source_byte_len: source.byte_len,
            source_modified_at_ms: source.modified_at_ms,
            source_content_sample: sampled_file_content_identity(source_path).with_context(
                || {
                    format!(
                        "sample isolated decoder source contents {}",
                        source_path.display()
                    )
                },
            )?,
            helper_path: helper_path.to_string_lossy().into_owned(),
            helper_byte_len: helper.as_ref().map(|fingerprint| fingerprint.byte_len),
            helper_modified_at_ms: helper.and_then(|fingerprint| fingerprint.modified_at_ms),
            helper_content_digest: full_file_content_digest(helper_path).ok(),
            provider_environment_identity: isolated_provider_environment_identity(),
            helper_protocol: helper_protocol.to_owned(),
            operation: operation.to_owned(),
        })
    }
}

/// Identifies values that can change which provider/pipeline the child opens.
///
/// This is intentionally a local safety-cache key, rather than catalog
/// provenance. A replacement helper, an explicit private-plugin replacement,
/// or a changed RAW/GPU policy must get a fresh safety observation instead of
/// inheriting a stale crash quarantine. The plugin's *path* is hashed so it is
/// not copied into the persisted record. An explicitly configured plugin also
/// contributes a full content digest. Discovery-mode private plugins remain a
/// limitation until the helper can report the selected module artifact itself.
fn isolated_provider_environment_identity() -> String {
    let mut hasher = blake3::Hasher::new();
    hash_environment_field(
        &mut hasher,
        "raw-pipeline",
        env::var_os(RAW_PIPELINE_ENVIRONMENT),
    );
    hash_environment_field(
        &mut hasher,
        "image-acceleration",
        env::var_os(IMAGE_ACCELERATION_ENVIRONMENT),
    );
    hash_environment_field(
        &mut hasher,
        "camera-profile-directory",
        env::var_os(CAMERA_PROFILE_DIRECTORY_ENVIRONMENT),
    );

    match env::var_os(PRIVATE_DECODER_PLUGIN_PATH_ENVIRONMENT) {
        Some(plugin_path) => {
            let plugin_path = PathBuf::from(plugin_path);
            hash_text_field(
                &mut hasher,
                "private-decoder-plugin-path",
                &plugin_path.to_string_lossy(),
            );
            match fingerprint_source(&plugin_path) {
                Ok(fingerprint) => {
                    hash_text_field(
                        &mut hasher,
                        "private-decoder-plugin-byte-len",
                        &fingerprint.byte_len.to_string(),
                    );
                    hash_text_field(
                        &mut hasher,
                        "private-decoder-plugin-modified-at-ms",
                        &fingerprint
                            .modified_at_ms
                            .map_or_else(|| "<unknown>".to_owned(), |value| value.to_string()),
                    );
                    match full_file_content_digest(&plugin_path) {
                        Ok(digest) => hash_text_field(
                            &mut hasher,
                            "private-decoder-plugin-content-digest",
                            &digest,
                        ),
                        Err(_) => hash_text_field(
                            &mut hasher,
                            "private-decoder-plugin-content-digest",
                            "<unavailable>",
                        ),
                    }
                }
                Err(_) => hash_text_field(
                    &mut hasher,
                    "private-decoder-plugin-fingerprint",
                    "<unavailable>",
                ),
            }
        }
        None => hash_text_field(&mut hasher, "private-decoder-plugin-path", "<discovered>"),
    }
    hasher.finalize().to_hex().to_string()
}

/// Stable implementation identity used by the outer desktop inspector when
/// the helper is enabled. The catalog may use this in a provider/preview
/// version because it reveals only a digest—not a private plugin path—while a
/// replacement helper or explicit private plugin cannot silently reuse an old
/// inspection/proxy cache entry after application restart.
pub(crate) fn isolated_helper_implementation_identity(
    helper_path: Option<&Path>,
) -> Result<String> {
    let mut hasher = blake3::Hasher::new();
    hash_text_field(
        &mut hasher,
        "isolated-provider-environment",
        &isolated_provider_environment_identity(),
    );
    match helper_path {
        Some(path) => {
            hash_text_field(&mut hasher, "helper-path", &path.to_string_lossy());
            match full_file_content_digest(path) {
                Ok(digest) => hash_text_field(&mut hasher, "helper-content-digest", &digest),
                Err(error) => {
                    hash_text_field(&mut hasher, "helper-content-digest", "<unavailable>");
                    hash_text_field(&mut hasher, "helper-open-error", &error.kind().to_string());
                }
            }
        }
        None => hash_text_field(&mut hasher, "helper-path", "<unconfigured>"),
    }
    Ok(hasher.finalize().to_hex().to_string())
}

fn full_file_content_digest(path: &Path) -> io::Result<String> {
    let mut file = fs::File::open(path)?;
    let mut hasher = blake3::Hasher::new();
    let mut buffer = [0_u8; 64 * 1024];
    loop {
        let count = file.read(&mut buffer)?;
        if count == 0 {
            break;
        }
        hasher.update(&buffer[..count]);
    }
    Ok(hasher.finalize().to_hex().to_string())
}

fn sampled_file_content_identity(path: &Path) -> io::Result<String> {
    let byte_len = fs::metadata(path)?.len();
    let mut file = fs::File::open(path)?;
    let sample_len = u64::try_from(SOURCE_SIGNATURE_SAMPLE_BYTES).expect("sample length fits u64");
    let middle = byte_len.saturating_sub(sample_len) / 2;
    let tail = byte_len.saturating_sub(sample_len);
    let mut offsets = [0_u64, middle, tail];
    offsets.sort_unstable();
    let mut hasher = blake3::Hasher::new();
    hasher.update(b"shadow-source-sample-v1");
    hasher.update(&byte_len.to_le_bytes());
    let mut previous = None;
    let mut buffer = [0_u8; SOURCE_SIGNATURE_SAMPLE_BYTES];
    for offset in offsets {
        if previous == Some(offset) {
            continue;
        }
        previous = Some(offset);
        file.seek(SeekFrom::Start(offset))?;
        let mut remaining = usize::try_from((byte_len - offset).min(sample_len))
            .expect("bounded sample length fits usize");
        hasher.update(&offset.to_le_bytes());
        while remaining > 0 {
            let chunk_len = remaining.min(buffer.len());
            let count = file.read(&mut buffer[..chunk_len])?;
            if count == 0 {
                break;
            }
            hasher.update(&buffer[..count]);
            remaining -= count;
        }
    }
    Ok(hasher.finalize().to_hex().to_string())
}

fn hash_environment_field(
    hasher: &mut blake3::Hasher,
    name: &str,
    value: Option<std::ffi::OsString>,
) {
    let value = value.map_or_else(
        || "<unset>".to_owned(),
        |value| value.to_string_lossy().into_owned(),
    );
    hash_text_field(hasher, name, &value);
}

fn hash_text_field(hasher: &mut blake3::Hasher, name: &str, value: &str) {
    hasher.update(&(name.len() as u64).to_le_bytes());
    hasher.update(name.as_bytes());
    hasher.update(&(value.len() as u64).to_le_bytes());
    hasher.update(value.as_bytes());
}

#[derive(Debug, Clone, Eq, PartialEq, serde::Serialize, serde::Deserialize)]
struct PersistedSafetyObservation {
    schema: u8,
    key: DecoderSafetyKey,
    observation: IsolatedDecodeObservation,
}

#[derive(Debug, Clone, Eq, PartialEq, serde::Serialize, serde::Deserialize)]
struct PersistedPreviewDevelopmentReceipt {
    schema: u8,
    key: DecoderSafetyKey,
    receipt: IsolatedPreviewDevelopmentReceipt,
}

#[derive(Debug, Clone, PartialEq, serde::Serialize, serde::Deserialize)]
struct PersistedMetadataSnapshot {
    schema: u8,
    key: DecoderSafetyKey,
    snapshot: IsolatedPhotoMetadataSnapshot,
}

#[derive(Debug, Clone, PartialEq, serde::Serialize, serde::Deserialize)]
struct PersistedDecoderSnapshot {
    schema: u8,
    key: DecoderSafetyKey,
    snapshot: IsolatedPhotoDecoderSnapshot,
}

#[derive(Debug, Default)]
struct DecoderSafetyRegistry {
    observations: Mutex<HashMap<DecoderSafetyKey, IsolatedDecodeObservation>>,
}

static DECODER_SAFETY_REGISTRY: OnceLock<DecoderSafetyRegistry> = OnceLock::new();

impl DecoderSafetyRegistry {
    fn shared() -> &'static Self {
        DECODER_SAFETY_REGISTRY.get_or_init(Self::default)
    }

    fn observation(
        &self,
        runtime_cache_root: &Path,
        key: &DecoderSafetyKey,
    ) -> Result<Option<IsolatedDecodeObservation>> {
        if let Some(observation) = self
            .observations
            .lock()
            .map_err(|_| anyhow::anyhow!("isolated decoder safety registry lock is poisoned"))?
            .get(key)
            .copied()
        {
            return Ok(Some(observation));
        }

        let path = safety_record_path(runtime_cache_root, key)?;
        let bytes = match fs::read(&path) {
            Ok(bytes) => bytes,
            Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(None),
            // This is a performance/safety hint cache, not source identity.
            // A permissions or transient I/O problem must not turn every
            // catalog scan into a hard failure.
            Err(_) => return Ok(None),
        };
        let record: PersistedSafetyObservation = match serde_json::from_slice(&bytes) {
            Ok(record) => record,
            // A partially written or future record is unknown, never an
            // identity match and never a process-wide ban.
            Err(_) => return Ok(None),
        };
        if record.schema != SAFETY_RECORD_SCHEMA || record.key != *key {
            // A cache record is advisory. Do not turn an older/corrupt cache
            // artifact into a global decoder ban, and never infer identity
            // from a filename alone.
            return Ok(None);
        }
        self.observations
            .lock()
            .map_err(|_| anyhow::anyhow!("isolated decoder safety registry lock is poisoned"))?
            .insert(key.clone(), record.observation);
        Ok(Some(record.observation))
    }

    fn record(
        &self,
        runtime_cache_root: &Path,
        key: &DecoderSafetyKey,
        observation: IsolatedDecodeObservation,
    ) -> Result<()> {
        self.observations
            .lock()
            .map_err(|_| anyhow::anyhow!("isolated decoder safety registry lock is poisoned"))?
            .insert(key.clone(), observation);
        let record = PersistedSafetyObservation {
            schema: SAFETY_RECORD_SCHEMA,
            key: key.clone(),
            observation,
        };
        let bytes =
            serde_json::to_vec(&record).context("serialize isolated decoder safety record")?;
        let path = safety_record_path(runtime_cache_root, key)?;
        write_atomic_cache_record(&path, &bytes)
    }
}

/// Combines the v1 child stages that can establish a *negative* main-process
/// safety fact. Neither successful stage promotes a source to native-safe;
/// only a crash/timeout makes the parent fail closed for this source revision.
pub(crate) fn native_decode_admission_after_isolated_stages(
    runtime_cache_root: &Path,
    source_path: &Path,
    helper_path: &Path,
) -> Result<NativeDecodeAdmission> {
    native_decode_admission_after_isolated_stages_for(
        DecoderSafetyRegistry::shared(),
        runtime_cache_root,
        source_path,
        helper_path,
    )
}

fn native_decode_admission_after_isolated_stages_for(
    registry: &DecoderSafetyRegistry,
    runtime_cache_root: &Path,
    source_path: &Path,
    helper_path: &Path,
) -> Result<NativeDecodeAdmission> {
    for key in [
        DecoderSafetyKey::metadata_probe(source_path, helper_path)?,
        DecoderSafetyKey::metadata_snapshot(source_path, helper_path)?,
        DecoderSafetyKey::decoder_snapshot(source_path, helper_path)?,
        DecoderSafetyKey::reference_proxy(source_path, helper_path)?,
    ] {
        let admission = native_decode_admission_from_observation(
            registry.observation(runtime_cache_root, &key)?,
        );
        if matches!(admission, NativeDecodeAdmission::Quarantined { .. }) {
            return Ok(admission);
        }
    }
    Ok(NativeDecodeAdmission::NotQuarantined)
}

fn native_decode_admission_from_observation(
    observation: Option<IsolatedDecodeObservation>,
) -> NativeDecodeAdmission {
    match observation {
        Some(observation) if observation.quarantines_main_process_native_decode() => {
            NativeDecodeAdmission::Quarantined { observation }
        }
        _ => NativeDecodeAdmission::NotQuarantined,
    }
}

/// Renders a bounded generated proxy in a child process.
///
/// The helper writes its JPEG into a short-lived cache-root path and sends only
/// small metadata over stdout. A bounded child wait converts a crash or hung
/// decoder into a per-photo preview failure while the desktop application
/// remains alive.
pub(crate) fn render_isolated_photo_reference_proxy(
    helper_path: &Path,
    runtime_cache_root: &Path,
    source_path: &Path,
    max_edge: u32,
    jpeg_quality: u8,
) -> Result<ProxyPayload> {
    let (proxy, output_path) = render_isolated_photo_reference_proxy_to_file(
        helper_path,
        runtime_cache_root,
        source_path,
        max_edge,
        jpeg_quality,
    )?;
    // The child alone owns this path and it has a UUID name. Cleanup is still
    // best-effort so an interrupted helper cannot grow the cache indefinitely.
    let _ = fs::remove_file(output_path);
    Ok(proxy)
}

/// Renders a bounded proxy and leaves its JPEG available to the caller briefly.
///
/// This is used only to bridge a private RAW provider into the ordinary, public
/// raster edit pipeline. The caller must remove `PathBuf` after opening it: the
/// C++ edit sessions copy their prepared pixels and retain no source file handle.
pub(crate) fn render_isolated_photo_reference_proxy_to_file(
    helper_path: &Path,
    runtime_cache_root: &Path,
    source_path: &Path,
    max_edge: u32,
    jpeg_quality: u8,
) -> Result<(ProxyPayload, PathBuf)> {
    if max_edge == 0 {
        bail!("generated proxy edge must be non-zero");
    }
    if !(1..=100).contains(&jpeg_quality) {
        bail!("generated proxy JPEG quality must be in 1..=100");
    }
    let safety_key = DecoderSafetyKey::reference_proxy(source_path, helper_path)?;
    if let Some(observation) =
        DecoderSafetyRegistry::shared().observation(runtime_cache_root, &safety_key)?
    {
        if observation.quarantines_main_process_native_decode() {
            bail!(
                "isolated RAW decoder is quarantined for this unchanged source after {}; change the source or helper before retrying native decode",
                observation.diagnostic_label()
            );
        }
    }
    let output_path = runtime_output_path(runtime_cache_root)?;
    let execution = execute_decode_helper(helper_path, |command| {
        command
            .arg("proxy")
            .arg(source_path)
            .arg(&output_path)
            .arg(max_edge.to_string())
            .arg(jpeg_quality.to_string());
    });
    let (result, observation) = match execution {
        Ok(HelperExecution::TimedOut(output)) => (
            Err(anyhow::anyhow!(
                "isolated RAW decoder timed out after {} seconds{}",
                HELPER_TIMEOUT.as_secs(),
                helper_stderr_suffix(&output.stderr)
            )),
            IsolatedDecodeObservation::TimedOut,
        ),
        Ok(HelperExecution::Completed(output)) => {
            let result = decode_helper_output(&output, &output_path, max_edge);
            let observation = if result.is_ok() {
                IsolatedDecodeObservation::LegacyStageSucceededUnproven
            } else if !output.status.success() && helper_exit_looks_like_crash(&output.status) {
                IsolatedDecodeObservation::ChildCrashed
            } else if !output.status.success() {
                IsolatedDecodeObservation::ChildRejected
            } else {
                IsolatedDecodeObservation::InvalidOutput
            };
            (result, observation)
        }
        Err(error) => (Err(error), IsolatedDecodeObservation::LaunchFailed),
    };
    record_safety_observation_if_source_is_current(
        runtime_cache_root,
        source_path,
        helper_path,
        &safety_key,
        observation,
    );
    match result {
        Ok(proxy) => Ok((proxy, output_path)),
        Err(error) => {
            let _ = fs::remove_file(&output_path);
            Err(error)
        }
    }
}

/// Returns the helper explicitly selected by the desktop shell, if present.
/// Keeping lookup outside the renderer makes tests and non-desktop clients
/// retain their direct public-provider behavior.
pub(crate) fn configured_helper_path() -> Option<PathBuf> {
    env::var_os(HELPER_PATH_ENVIRONMENT).map(PathBuf::from)
}

/// Runs only provider opening plus metadata/capability/preview enumeration in
/// the child process.
///
/// This preflight protects the desktop scanner from the most common native
/// failure boundary (`provider->open` and metadata parsing). It does *not*
/// certify RawFrame, actual embedded preview bytes, detail, or export: each of
/// those needs its own child stage/receipt before it can be called safe.
pub(crate) fn probe_isolated_photo_metadata(
    helper_path: &Path,
    runtime_cache_root: &Path,
    source_path: &Path,
) -> Result<()> {
    let safety_key = DecoderSafetyKey::metadata_probe(source_path, helper_path)?;
    if let Some(observation) =
        DecoderSafetyRegistry::shared().observation(runtime_cache_root, &safety_key)?
    {
        match observation {
            IsolatedDecodeObservation::LegacyStageSucceededUnproven => return Ok(()),
            observation if observation.quarantines_main_process_native_decode() => {
                bail!(
                    "isolated RAW metadata probe is quarantined for this unchanged source after {}; change the source or helper before retrying",
                    observation.diagnostic_label()
                );
            }
            // Ordinary decoder rejection and malformed protocol output are
            // not permanence claims. Let the helper retry in case a local
            // private provider or its sidecar data has been repaired.
            _ => {}
        }
    }

    let execution = execute_decode_helper(helper_path, |command| {
        command.arg("probe").arg(source_path);
    });
    let (result, observation) = match execution {
        Ok(HelperExecution::TimedOut(output)) => (
            Err(anyhow::anyhow!(
                "isolated RAW metadata probe timed out after {} seconds{}",
                HELPER_TIMEOUT.as_secs(),
                helper_stderr_suffix(&output.stderr)
            )),
            IsolatedDecodeObservation::TimedOut,
        ),
        Ok(HelperExecution::Completed(output)) => {
            let result = decode_probe_output(&output);
            let observation = if result.is_ok() {
                IsolatedDecodeObservation::LegacyStageSucceededUnproven
            } else if !output.status.success() && helper_exit_looks_like_crash(&output.status) {
                IsolatedDecodeObservation::ChildCrashed
            } else if !output.status.success() {
                IsolatedDecodeObservation::ChildRejected
            } else {
                IsolatedDecodeObservation::InvalidOutput
            };
            (result, observation)
        }
        Err(error) => (Err(error), IsolatedDecodeObservation::LaunchFailed),
    };
    record_safety_observation_if_source_is_current(
        runtime_cache_root,
        source_path,
        helper_path,
        &safety_key,
        observation,
    );
    result
}

/// Opens a source and copies its bounded metadata snapshot from the helper.
///
/// This is the metadata-bearing counterpart to the v1 `probe` protocol. It is
/// intended for Catalog and optics callers that otherwise would reopen a RAW
/// merely because an older catalog row lacks EXIF. It returns no preview bytes
/// and no RawFrame, and successful completion still does not certify a later
/// in-process detail or export decode.
pub(crate) fn snapshot_isolated_photo_metadata(
    helper_path: &Path,
    runtime_cache_root: &Path,
    source_path: &Path,
) -> Result<IsolatedPhotoMetadataSnapshot> {
    let safety_key = DecoderSafetyKey::metadata_snapshot(source_path, helper_path)?;
    if let Some(observation) =
        DecoderSafetyRegistry::shared().observation(runtime_cache_root, &safety_key)?
    {
        if observation.quarantines_main_process_native_decode() {
            bail!(
                "isolated RAW metadata snapshot is quarantined for this unchanged source after {}; change the source or helper before retrying",
                observation.diagnostic_label()
            );
        }
    }
    if let Some(snapshot) = read_metadata_snapshot(runtime_cache_root, &safety_key)? {
        return Ok(snapshot);
    }

    let nonce = Uuid::now_v7().simple().to_string();
    let execution = execute_decode_helper(helper_path, |command| {
        command
            .arg("metadata-snapshot")
            .arg(source_path)
            .arg(&nonce);
    });
    let (result, observation) = match execution {
        Ok(HelperExecution::TimedOut(output)) => (
            Err(anyhow::anyhow!(
                "isolated RAW metadata snapshot timed out after {} seconds{}",
                HELPER_TIMEOUT.as_secs(),
                helper_stderr_suffix(&output.stderr)
            )),
            IsolatedDecodeObservation::TimedOut,
        ),
        Ok(HelperExecution::Completed(output)) => {
            let result = decode_metadata_snapshot_output(&output, &nonce);
            let observation = if result.is_ok() {
                IsolatedDecodeObservation::LegacyStageSucceededUnproven
            } else if !output.status.success() && helper_exit_looks_like_crash(&output.status) {
                IsolatedDecodeObservation::ChildCrashed
            } else if !output.status.success() {
                IsolatedDecodeObservation::ChildRejected
            } else {
                IsolatedDecodeObservation::InvalidOutput
            };
            (result, observation)
        }
        Err(error) => (Err(error), IsolatedDecodeObservation::LaunchFailed),
    };

    let source_is_current = safety_key_is_current(source_path, helper_path, &safety_key);
    if source_is_current {
        // As with all helper-derived records, cache failure only loses a
        // future reuse opportunity; it never suppresses the child diagnosis.
        let _ =
            DecoderSafetyRegistry::shared().record(runtime_cache_root, &safety_key, observation);
        if let Ok(snapshot) = &result {
            let _ = write_metadata_snapshot(runtime_cache_root, &safety_key, snapshot);
        }
    }
    if result.is_ok() && !source_is_current {
        bail!("source or helper changed while the isolated metadata snapshot was running");
    }
    result
}

/// Opens a RAW/provider source entirely in the helper and copies the bounded
/// descriptor snapshot needed by the catalog worker. No preview bytes,
/// `RawFrame`, source path, or developed RGB crosses this protocol. A caller
/// must treat successful completion as evidence about this one inspection
/// stage—not permission to reopen the RAW in the desktop process.
pub(crate) fn snapshot_isolated_photo_decoder(
    helper_path: &Path,
    runtime_cache_root: &Path,
    source_path: &Path,
) -> Result<IsolatedPhotoDecoderSnapshot> {
    let safety_key = DecoderSafetyKey::decoder_snapshot(source_path, helper_path)?;
    if let Some(observation) =
        DecoderSafetyRegistry::shared().observation(runtime_cache_root, &safety_key)?
    {
        if observation.quarantines_main_process_native_decode() {
            bail!(
                "isolated RAW decoder inspection is quarantined for this unchanged source after {}; change the source or helper before retrying",
                observation.diagnostic_label()
            );
        }
    }
    if let Some(snapshot) = read_decoder_snapshot(runtime_cache_root, &safety_key)? {
        return Ok(snapshot);
    }

    let nonce = Uuid::now_v7().simple().to_string();
    let execution = execute_decode_helper(helper_path, |command| {
        command.arg("decoder-snapshot").arg(source_path).arg(&nonce);
    });
    let (result, observation) = match execution {
        Ok(HelperExecution::TimedOut(output)) => (
            Err(anyhow::anyhow!(
                "isolated RAW decoder inspection timed out after {} seconds{}",
                HELPER_TIMEOUT.as_secs(),
                helper_stderr_suffix(&output.stderr)
            )),
            IsolatedDecodeObservation::TimedOut,
        ),
        Ok(HelperExecution::Completed(output)) => {
            let result = decode_decoder_snapshot_output(&output, &nonce);
            let observation = if result.is_ok() {
                IsolatedDecodeObservation::LegacyStageSucceededUnproven
            } else if !output.status.success() && helper_exit_looks_like_crash(&output.status) {
                IsolatedDecodeObservation::ChildCrashed
            } else if !output.status.success() {
                IsolatedDecodeObservation::ChildRejected
            } else {
                IsolatedDecodeObservation::InvalidOutput
            };
            (result, observation)
        }
        Err(error) => (Err(error), IsolatedDecodeObservation::LaunchFailed),
    };

    let source_is_current = safety_key_is_current(source_path, helper_path, &safety_key);
    if source_is_current {
        // Cache publication is advisory. Preserve the actual helper result if
        // a cache volume becomes unavailable between decode and write.
        let _ =
            DecoderSafetyRegistry::shared().record(runtime_cache_root, &safety_key, observation);
        if let Ok(snapshot) = &result {
            let _ = write_decoder_snapshot(runtime_cache_root, &safety_key, snapshot);
        }
    }
    if result.is_ok() && !source_is_current {
        bail!("source or helper changed while the isolated decoder inspection was running");
    }
    result
}

/// Executes the actual bounded preview RAW-development stage in the helper and
/// returns its opaque route receipt.
///
/// This is deliberately stronger than `probe_isolated_photo_metadata`: it
/// exercises source-provider selection and `develop_source_reference` under
/// the effective preview policy. It still does *not* certify a full-detail or
/// export open, and it does not move any pixels into the desktop process.
pub(crate) fn probe_isolated_preview_development(
    helper_path: &Path,
    runtime_cache_root: &Path,
    source_path: &Path,
    max_edge: u32,
) -> Result<IsolatedPreviewDevelopmentReceipt> {
    if max_edge == 0 {
        bail!("isolated preview-development edge must be non-zero");
    }
    let safety_key = DecoderSafetyKey::preview_development(source_path, helper_path, max_edge)?;
    if let Some(observation) =
        DecoderSafetyRegistry::shared().observation(runtime_cache_root, &safety_key)?
    {
        if observation.quarantines_main_process_native_decode() {
            bail!(
                "isolated preview-development probe is quarantined for this unchanged source after {}; change the source or helper before retrying",
                observation.diagnostic_label()
            );
        }
    }
    if let Some(receipt) = read_preview_development_receipt(runtime_cache_root, &safety_key)? {
        return Ok(receipt);
    }

    let nonce = Uuid::now_v7().simple().to_string();
    let execution = execute_decode_helper(helper_path, |command| {
        command
            .arg("preview-receipt")
            .arg(source_path)
            .arg(max_edge.to_string())
            .arg(&nonce);
    });
    let (result, observation) = match execution {
        Ok(HelperExecution::TimedOut(output)) => (
            Err(anyhow::anyhow!(
                "isolated preview-development probe timed out after {} seconds{}",
                HELPER_TIMEOUT.as_secs(),
                helper_stderr_suffix(&output.stderr)
            )),
            IsolatedDecodeObservation::TimedOut,
        ),
        Ok(HelperExecution::Completed(output)) => {
            let result = decode_preview_development_output(&output, &nonce);
            let observation = if result.is_ok() {
                IsolatedDecodeObservation::LegacyStageSucceededUnproven
            } else if !output.status.success() && helper_exit_looks_like_crash(&output.status) {
                IsolatedDecodeObservation::ChildCrashed
            } else if !output.status.success() {
                IsolatedDecodeObservation::ChildRejected
            } else {
                IsolatedDecodeObservation::InvalidOutput
            };
            (result, observation)
        }
        Err(error) => (Err(error), IsolatedDecodeObservation::LaunchFailed),
    };

    let source_is_current = safety_key_is_current(source_path, helper_path, &safety_key);
    if source_is_current {
        // As with the v1 safety state, cache publication is advisory. A
        // write failure does not hide the current child diagnosis or turn an
        // implementation receipt into a source identity claim.
        let _ =
            DecoderSafetyRegistry::shared().record(runtime_cache_root, &safety_key, observation);
        if let Ok(receipt) = &result {
            let _ = write_preview_development_receipt(runtime_cache_root, &safety_key, receipt);
        }
    }

    if result.is_ok() && !source_is_current {
        bail!("source or helper changed while the isolated preview-development probe was running");
    }
    result
}

#[derive(Debug)]
struct HelperProcessOutput {
    status: ExitStatus,
    stdout: Vec<u8>,
    stderr: Vec<u8>,
}

#[derive(Debug)]
enum HelperExecution {
    Completed(HelperProcessOutput),
    TimedOut(HelperProcessOutput),
}

fn execute_decode_helper(
    helper_path: &Path,
    configure: impl FnOnce(&mut Command),
) -> Result<HelperExecution> {
    execute_decode_helper_with_timeout(helper_path, HELPER_TIMEOUT, configure)
}

fn execute_decode_helper_with_timeout(
    helper_path: &Path,
    timeout: Duration,
    configure: impl FnOnce(&mut Command),
) -> Result<HelperExecution> {
    let mut command = Command::new(helper_path);
    command
        // The desktop host must not load a private native SDK. The helper is
        // its intentional isolation boundary, so it alone is allowed to probe
        // an installed private provider after the public decoder declines.
        .env_remove(DISABLE_PRIVATE_DECODER_ENVIRONMENT)
        // A malformed third-party provider must not be able to fill an
        // unbounded pipe before the timeout can be observed. Reader threads
        // keep both pipes draining while retaining a capped diagnostic tail.
        .stdout(Stdio::piped())
        .stderr(Stdio::piped());
    configure_helper_process_group(&mut command);
    configure(&mut command);
    let mut child = command
        .spawn()
        .with_context(|| format!("start isolated RAW decoder {}", helper_path.display()))?;
    let stdout = child
        .stdout
        .take()
        .ok_or_else(|| anyhow::anyhow!("isolated RAW decoder did not expose stdout"))?;
    let stderr = child
        .stderr
        .take()
        .ok_or_else(|| anyhow::anyhow!("isolated RAW decoder did not expose stderr"))?;
    let stdout_reader = spawn_helper_pipe_reader(stdout);
    let stderr_reader = spawn_helper_pipe_reader(stderr);
    let started_at = Instant::now();
    let mut timed_out = false;
    let status = loop {
        match child.try_wait() {
            Ok(Some(status)) => break status,
            Ok(None) if started_at.elapsed() >= timeout => {
                timed_out = true;
                terminate_helper_process_tree(&mut child);
                break child
                    .wait()
                    .context("wait for timed-out isolated RAW decoder")?;
            }
            Ok(None) => thread::sleep(HELPER_POLL_INTERVAL),
            Err(error) => {
                terminate_helper_process_tree(&mut child);
                let _ = child.wait();
                return Err(error).context("poll isolated RAW decoder");
            }
        }
    };
    // Do not use JoinHandle::join here: a private SDK may have spawned a
    // descendant which inherited one pipe. A successful direct-child exit
    // must not make the desktop block forever waiting for that unrelated EOF.
    let drain_deadline = Instant::now() + HELPER_PIPE_DRAIN_TIMEOUT;
    let stdout = receive_helper_pipe_until(&stdout_reader, "stdout", drain_deadline)?;
    let stderr = receive_helper_pipe_until(&stderr_reader, "stderr", drain_deadline)?;
    if stdout.is_none() || stderr.is_none() {
        // On Unix this kills the dedicated helper process group, including
        // ordinary descendants that kept a pipe open. On other platforms the
        // direct child has already been reaped, but the parent still returns
        // on the same bounded deadline instead of joining a stuck reader.
        terminate_helper_process_tree(&mut child);
        return Ok(HelperExecution::TimedOut(HelperProcessOutput {
            status,
            stdout: stdout.unwrap_or_default(),
            stderr: stderr.unwrap_or_default(),
        }));
    }
    let output = HelperProcessOutput {
        status,
        stdout: stdout.expect("checked above"),
        stderr: stderr.expect("checked above"),
    };
    Ok(if timed_out {
        HelperExecution::TimedOut(output)
    } else {
        HelperExecution::Completed(output)
    })
}

fn configure_helper_process_group(command: &mut Command) {
    #[cfg(unix)]
    {
        // The helper gets its own process group so a timeout can terminate
        // regular decoder descendants that inherited stdout/stderr as well.
        use std::os::unix::process::CommandExt as _;
        command.process_group(0);
    }
    #[cfg(not(unix))]
    let _ = command;
}

fn terminate_helper_process_tree(child: &mut Child) {
    #[cfg(unix)]
    {
        // The group id is the helper's PID because configure_helper_process_group
        // requested process_group(0). Ignore ESRCH: the child can exit in the
        // narrow interval between try_wait and this cleanup.
        if let Ok(process_group) = i32::try_from(child.id()) {
            // SAFETY: `killpg` receives a positive PID-derived group id and
            // SIGKILL. The child was spawned into its own group above, so this
            // never targets the desktop process group.
            let _ = unsafe { killpg(process_group, 9) };
        }
    }
    let _ = child.kill();
}

fn read_capped_stream(mut stream: impl Read) -> io::Result<Vec<u8>> {
    let mut bytes = Vec::new();
    let mut buffer = [0_u8; 4_096];
    loop {
        let count = stream.read(&mut buffer)?;
        if count == 0 {
            break;
        }
        let remaining = MAX_HELPER_STREAM_BYTES.saturating_sub(bytes.len());
        bytes.extend_from_slice(&buffer[..count.min(remaining)]);
    }
    Ok(bytes)
}

fn spawn_helper_pipe_reader(
    stream: impl Read + Send + 'static,
) -> mpsc::Receiver<io::Result<Vec<u8>>> {
    let (sender, receiver) = mpsc::sync_channel(1);
    thread::spawn(move || {
        // If the parent returns after the bounded drain deadline, dropping the
        // receiver is intentional: the reader owns no desktop state and must
        // never keep the UI waiting for a hostile inherited descriptor.
        let _ = sender.send(read_capped_stream(stream));
    });
    receiver
}

fn receive_helper_pipe_until(
    reader: &mpsc::Receiver<io::Result<Vec<u8>>>,
    name: &'static str,
    deadline: Instant,
) -> Result<Option<Vec<u8>>> {
    match reader.recv_timeout(deadline.saturating_duration_since(Instant::now())) {
        Ok(result) => result
            .with_context(|| format!("read isolated RAW decoder {name}"))
            .map(Some),
        Err(mpsc::RecvTimeoutError::Timeout) => Ok(None),
        Err(mpsc::RecvTimeoutError::Disconnected) => {
            bail!("isolated RAW decoder {name} reader disconnected")
        }
    }
}

fn helper_exit_looks_like_crash(status: &ExitStatus) -> bool {
    // Unix reports signal termination as `None`. Some launchers translate
    // SIGABRT/SIGSEGV to conventional 134/139 exit codes; Windows exception
    // statuses are signed negative `i32`s. A non-zero ordinary `return 1`
    // stays a normal decoder rejection and is not a native-crash quarantine.
    status.code().is_none()
        || status
            .code()
            .is_some_and(|code| matches!(code, 134 | 139) || code < 0)
}

fn helper_stderr_suffix(stderr: &[u8]) -> String {
    let detail = String::from_utf8_lossy(stderr).trim().to_owned();
    if detail.is_empty() {
        String::new()
    } else {
        format!(": {detail}")
    }
}

fn safety_record_path(runtime_cache_root: &Path, key: &DecoderSafetyKey) -> Result<PathBuf> {
    let key = serde_json::to_vec(key).context("serialize isolated decoder safety key")?;
    let digest = blake3::hash(&key).to_hex();
    Ok(runtime_cache_root
        .join("decode-helper")
        .join("safety")
        .join(format!("{digest}.json")))
}

fn preview_development_receipt_path(
    runtime_cache_root: &Path,
    key: &DecoderSafetyKey,
) -> Result<PathBuf> {
    let key = serde_json::to_vec(key).context("serialize isolated preview-development key")?;
    let digest = blake3::hash(&key).to_hex();
    Ok(runtime_cache_root
        .join("decode-helper")
        .join("preview-development-receipts")
        .join(format!("{digest}.json")))
}

fn metadata_snapshot_path(runtime_cache_root: &Path, key: &DecoderSafetyKey) -> Result<PathBuf> {
    let key = serde_json::to_vec(key).context("serialize isolated metadata snapshot key")?;
    let digest = blake3::hash(&key).to_hex();
    Ok(runtime_cache_root
        .join("decode-helper")
        .join("metadata-snapshots")
        .join(format!("{digest}.json")))
}

fn decoder_snapshot_path(runtime_cache_root: &Path, key: &DecoderSafetyKey) -> Result<PathBuf> {
    let key = serde_json::to_vec(key).context("serialize isolated decoder snapshot key")?;
    let digest = blake3::hash(&key).to_hex();
    Ok(runtime_cache_root
        .join("decode-helper")
        .join("decoder-snapshots")
        .join(format!("{digest}.json")))
}

fn read_preview_development_receipt(
    runtime_cache_root: &Path,
    key: &DecoderSafetyKey,
) -> Result<Option<IsolatedPreviewDevelopmentReceipt>> {
    let path = preview_development_receipt_path(runtime_cache_root, key)?;
    let bytes = match fs::read(&path) {
        Ok(bytes) => bytes,
        Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(None),
        // This record only saves a repeated child development. It never
        // represents source identity, so an unreadable cache is a miss.
        Err(_) => return Ok(None),
    };
    let record: PersistedPreviewDevelopmentReceipt = match serde_json::from_slice(&bytes) {
        Ok(record) => record,
        Err(_) => return Ok(None),
    };
    if record.schema != PREVIEW_DEVELOPMENT_RECEIPT_SCHEMA || record.key != *key {
        return Ok(None);
    }
    Ok(Some(record.receipt))
}

fn write_preview_development_receipt(
    runtime_cache_root: &Path,
    key: &DecoderSafetyKey,
    receipt: &IsolatedPreviewDevelopmentReceipt,
) -> Result<()> {
    let record = PersistedPreviewDevelopmentReceipt {
        schema: PREVIEW_DEVELOPMENT_RECEIPT_SCHEMA,
        key: key.clone(),
        receipt: receipt.clone(),
    };
    let bytes =
        serde_json::to_vec(&record).context("serialize isolated preview-development receipt")?;
    let path = preview_development_receipt_path(runtime_cache_root, key)?;
    write_atomic_cache_record(&path, &bytes)
}

fn read_metadata_snapshot(
    runtime_cache_root: &Path,
    key: &DecoderSafetyKey,
) -> Result<Option<IsolatedPhotoMetadataSnapshot>> {
    let path = metadata_snapshot_path(runtime_cache_root, key)?;
    let bytes = match fs::read(&path) {
        Ok(bytes) => bytes,
        Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(None),
        // This is an acceleration cache. A permission, partial-write, or
        // future-schema problem must trigger a fresh child read, not convert
        // into a global metadata failure.
        Err(_) => return Ok(None),
    };
    let record: PersistedMetadataSnapshot = match serde_json::from_slice(&bytes) {
        Ok(record) => record,
        Err(_) => return Ok(None),
    };
    if record.schema != METADATA_SNAPSHOT_SCHEMA || record.key != *key {
        return Ok(None);
    }
    Ok(Some(record.snapshot))
}

fn write_metadata_snapshot(
    runtime_cache_root: &Path,
    key: &DecoderSafetyKey,
    snapshot: &IsolatedPhotoMetadataSnapshot,
) -> Result<()> {
    let record = PersistedMetadataSnapshot {
        schema: METADATA_SNAPSHOT_SCHEMA,
        key: key.clone(),
        snapshot: snapshot.clone(),
    };
    let bytes = serde_json::to_vec(&record).context("serialize isolated metadata snapshot")?;
    let path = metadata_snapshot_path(runtime_cache_root, key)?;
    write_atomic_cache_record(&path, &bytes)
}

fn read_decoder_snapshot(
    runtime_cache_root: &Path,
    key: &DecoderSafetyKey,
) -> Result<Option<IsolatedPhotoDecoderSnapshot>> {
    let path = decoder_snapshot_path(runtime_cache_root, key)?;
    let bytes = match fs::read(&path) {
        Ok(bytes) => bytes,
        Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(None),
        // This is an acceleration/provenance cache only. A bad record must
        // schedule a fresh isolated read, never become a source identity fact.
        Err(_) => return Ok(None),
    };
    let record: PersistedDecoderSnapshot = match serde_json::from_slice(&bytes) {
        Ok(record) => record,
        Err(_) => return Ok(None),
    };
    if record.schema != DECODER_SNAPSHOT_SCHEMA || record.key != *key {
        return Ok(None);
    }
    Ok(Some(record.snapshot))
}

fn write_decoder_snapshot(
    runtime_cache_root: &Path,
    key: &DecoderSafetyKey,
    snapshot: &IsolatedPhotoDecoderSnapshot,
) -> Result<()> {
    let record = PersistedDecoderSnapshot {
        schema: DECODER_SNAPSHOT_SCHEMA,
        key: key.clone(),
        snapshot: snapshot.clone(),
    };
    let bytes = serde_json::to_vec(&record).context("serialize isolated decoder snapshot")?;
    let path = decoder_snapshot_path(runtime_cache_root, key)?;
    write_atomic_cache_record(&path, &bytes)
}

fn write_atomic_cache_record(path: &Path, bytes: &[u8]) -> Result<()> {
    let directory = path
        .parent()
        .ok_or_else(|| anyhow::anyhow!("isolated decoder safety path has no parent"))?;
    fs::create_dir_all(directory).with_context(|| {
        format!(
            "create isolated decoder safety directory {}",
            directory.display()
        )
    })?;
    let temporary = directory.join(format!(".{}.{}.tmp", Uuid::now_v7(), "safety"));
    fs::write(&temporary, bytes).with_context(|| {
        format!(
            "write isolated decoder safety record {}",
            temporary.display()
        )
    })?;
    if let Err(error) = fs::rename(&temporary, path) {
        // Windows cannot replace an existing destination with `rename`. This
        // is an advisory cache record (a miss means unknown, never a false
        // identity), so a narrow replace fallback is acceptable here.
        if error.kind() != io::ErrorKind::AlreadyExists {
            let _ = fs::remove_file(&temporary);
            return Err(error).with_context(|| {
                format!("publish isolated decoder safety record {}", path.display())
            });
        }
        let _ = fs::remove_file(path);
        if let Err(error) = fs::rename(&temporary, path) {
            let _ = fs::remove_file(&temporary);
            return Err(error).with_context(|| {
                format!("replace isolated decoder safety record {}", path.display())
            });
        }
    }
    Ok(())
}

fn record_safety_observation_if_source_is_current(
    runtime_cache_root: &Path,
    source_path: &Path,
    helper_path: &Path,
    before: &DecoderSafetyKey,
    observation: IsolatedDecodeObservation,
) {
    if !safety_key_is_current(source_path, helper_path, before) {
        return;
    }
    // A cache-record failure must never hide the original decoder diagnosis.
    // The in-memory registry is updated before the best-effort disk publish,
    // so this process still avoids a repeat child crash.
    let _ = DecoderSafetyRegistry::shared().record(runtime_cache_root, before, observation);
}

fn safety_key_is_current(
    source_path: &Path,
    helper_path: &Path,
    before: &DecoderSafetyKey,
) -> bool {
    let Ok(after) = DecoderSafetyKey::for_operation(
        source_path,
        helper_path,
        &before.helper_protocol,
        &before.operation,
    ) else {
        return false;
    };
    if after != *before {
        return false;
    }
    true
}

fn runtime_output_path(runtime_cache_root: &Path) -> Result<PathBuf> {
    let directory = runtime_cache_root.join("decode-helper");
    fs::create_dir_all(&directory).with_context(|| {
        format!(
            "create isolated decode cache directory {}",
            directory.display()
        )
    })?;
    Ok(directory.join(format!("{}.jpg", Uuid::now_v7())))
}

fn decode_helper_output(
    output: &HelperProcessOutput,
    output_path: &Path,
    max_edge: u32,
) -> Result<ProxyPayload> {
    if !output.status.success() {
        let status = output.status.to_string();
        bail!(
            "isolated RAW decoder exited with {status}{}",
            helper_stderr_suffix(&output.stderr)
        );
    }
    let dimensions = parse_helper_dimensions(&output.stdout)?;
    if dimensions.width == 0 || dimensions.height == 0 {
        bail!("isolated RAW decoder returned zero-sized proxy dimensions");
    }
    if dimensions.width.max(dimensions.height) > max_edge {
        bail!("isolated RAW decoder exceeded requested proxy edge");
    }
    let metadata = fs::metadata(output_path)
        .with_context(|| format!("read isolated proxy output {}", output_path.display()))?;
    if metadata.len() == 0 || metadata.len() > MAX_PROXY_BYTES {
        bail!("isolated RAW decoder produced an invalid proxy byte length");
    }
    let bytes = fs::read(output_path)
        .with_context(|| format!("read isolated proxy output {}", output_path.display()))?;
    if bytes.len() < 4 || bytes[..2] != [0xff, 0xd8] || bytes[bytes.len() - 2..] != [0xff, 0xd9] {
        bail!("isolated RAW decoder produced a non-JPEG proxy");
    }
    Ok(ProxyPayload {
        dimensions,
        codec: PreviewCodec::Jpeg,
        bits_per_channel: 8,
        channels: 3,
        bytes,
    })
}

fn decode_probe_output(output: &HelperProcessOutput) -> Result<()> {
    if !output.status.success() {
        bail!(
            "isolated RAW metadata probe exited with {}{}",
            output.status,
            helper_stderr_suffix(&output.stderr)
        );
    }
    parse_probe_protocol(&output.stdout)
}

fn decode_preview_development_output(
    output: &HelperProcessOutput,
    nonce: &str,
) -> Result<IsolatedPreviewDevelopmentReceipt> {
    if !output.status.success() {
        bail!(
            "isolated preview-development probe exited with {}{}",
            output.status,
            helper_stderr_suffix(&output.stderr)
        );
    }
    parse_preview_development_protocol(&output.stdout, nonce)
}

fn decode_metadata_snapshot_output(
    output: &HelperProcessOutput,
    nonce: &str,
) -> Result<IsolatedPhotoMetadataSnapshot> {
    if !output.status.success() {
        bail!(
            "isolated RAW metadata snapshot exited with {}{}",
            output.status,
            helper_stderr_suffix(&output.stderr)
        );
    }
    parse_metadata_snapshot_protocol(&output.stdout, nonce)
}

fn decode_decoder_snapshot_output(
    output: &HelperProcessOutput,
    nonce: &str,
) -> Result<IsolatedPhotoDecoderSnapshot> {
    if !output.status.success() {
        bail!(
            "isolated RAW decoder inspection exited with {}{}",
            output.status,
            helper_stderr_suffix(&output.stderr)
        );
    }
    parse_decoder_snapshot_protocol(&output.stdout, nonce)
}

fn parse_probe_protocol(stdout: &[u8]) -> Result<()> {
    let response = std::str::from_utf8(stdout)
        .context("decode isolated RAW metadata probe output")?
        .trim();
    if response != format!("{METADATA_PROBE_PROTOCOL} open-metadata") {
        bail!("isolated RAW metadata probe returned an invalid protocol response");
    }
    Ok(())
}

fn parse_preview_development_protocol(
    stdout: &[u8],
    expected_nonce: &str,
) -> Result<IsolatedPreviewDevelopmentReceipt> {
    if expected_nonce.is_empty() {
        bail!("isolated preview-development probe nonce must be non-empty");
    }
    let response =
        std::str::from_utf8(stdout).context("decode isolated preview-development probe output")?;
    let fields = response.split_whitespace().collect::<Vec<_>>();
    if fields.len() != 9
        || fields[0] != PREVIEW_DEVELOPMENT_PROBE_PROTOCOL
        || fields[1] != "preview-development"
        || fields[2] != expected_nonce
    {
        bail!("isolated preview-development probe returned an invalid protocol response");
    }
    let raw_pipeline_path = fields[3]
        .parse::<u8>()
        .context("parse isolated preview-development pipeline path")?;
    if raw_pipeline_path > 2 {
        bail!("isolated preview-development probe returned an unknown pipeline path");
    }
    Ok(IsolatedPreviewDevelopmentReceipt {
        raw_pipeline_path,
        source_provider_id: decode_receipt_identity_field(fields[4], "source provider id")?,
        source_provider_version: decode_receipt_identity_field(
            fields[5],
            "source provider version",
        )?,
        pipeline_receipt_identity: decode_receipt_identity_field(
            fields[6],
            "pipeline receipt identity",
        )?,
        requested_plan_identity: decode_receipt_identity_field(
            fields[7],
            "requested plan identity",
        )?,
        effective_plan_identity: decode_receipt_identity_field(
            fields[8],
            "effective plan identity",
        )?,
    })
}

fn parse_metadata_snapshot_protocol(
    stdout: &[u8],
    expected_nonce: &str,
) -> Result<IsolatedPhotoMetadataSnapshot> {
    if expected_nonce.is_empty() {
        bail!("isolated metadata snapshot nonce must be non-empty");
    }
    let response =
        std::str::from_utf8(stdout).context("decode isolated RAW metadata snapshot output")?;
    let fields = response.split_whitespace().collect::<Vec<_>>();
    if fields.len() != METADATA_SNAPSHOT_FIELD_COUNT
        || fields[0] != METADATA_SNAPSHOT_PROTOCOL
        || fields[1] != "metadata-snapshot"
        || fields[2] != expected_nonce
    {
        bail!("isolated RAW metadata snapshot returned an invalid protocol response");
    }

    Ok(IsolatedPhotoMetadataSnapshot {
        router_provider_id: decode_receipt_identity_field(fields[3], "router provider id")?,
        router_provider_version: decode_receipt_identity_field(
            fields[4],
            "router provider version",
        )?,
        metadata: parse_raw_metadata_snapshot_fields(
            &fields[5..],
            MAX_METADATA_TEXT_BYTES,
            "isolated RAW metadata snapshot",
        )?,
    })
}

fn parse_raw_metadata_snapshot_fields(
    fields: &[&str],
    maximum_text_bytes: usize,
    protocol_label: &str,
) -> Result<RawMetadataSnapshot> {
    if fields.len() != RAW_METADATA_SNAPSHOT_FIELD_COUNT {
        bail!("{protocol_label} returned an invalid metadata field count");
    }
    let decode_text = |index: usize, label: &str| {
        decode_protocol_hex_text_field(fields[index], label, maximum_text_bytes, true)
    };
    let dng_version = decode_text(4, "DNG version")?;
    Ok(RawMetadataSnapshot {
        make: decode_text(0, "make")?,
        model: decode_text(1, "model")?,
        normalized_make: decode_text(2, "normalized make")?,
        normalized_model: decode_text(3, "normalized model")?,
        dng_version: (!dng_version.is_empty()).then_some(dng_version),
        raw_count: parse_metadata_u32(fields[5], "raw count")?,
        raw_dimensions: ImageDimensions {
            width: parse_metadata_u32(fields[6], "raw width")?,
            height: parse_metadata_u32(fields[7], "raw height")?,
        },
        image_dimensions: ImageDimensions {
            width: parse_metadata_u32(fields[8], "image width")?,
            height: parse_metadata_u32(fields[9], "image height")?,
        },
        margins: ImageMargins {
            left: parse_metadata_u32(fields[10], "left margin")?,
            top: parse_metadata_u32(fields[11], "top margin")?,
            right: parse_metadata_u32(fields[12], "right margin")?,
            bottom: parse_metadata_u32(fields[13], "bottom margin")?,
        },
        orientation: parse_metadata_i32(fields[14], "orientation")?,
        cfa_pattern: decode_text(15, "CFA pattern")?,
        sensor_colors: parse_metadata_u32(fields[16], "sensor colors")?,
        sensor_bits: parse_metadata_u32(fields[17], "sensor bits")?,
        black_level: parse_metadata_u32(fields[18], "black level")?,
        white_level: parse_metadata_u32(fields[19], "white level")?,
        as_shot_neutral: [
            parse_metadata_f64(fields[20], "as-shot neutral red")?,
            parse_metadata_f64(fields[21], "as-shot neutral green 1")?,
            parse_metadata_f64(fields[22], "as-shot neutral blue")?,
            parse_metadata_f64(fields[23], "as-shot neutral green 2")?,
        ],
        baseline_exposure: parse_metadata_f64(fields[24], "baseline exposure")?,
        iso_speed: parse_metadata_f64(fields[25], "ISO speed")?,
        exposure_time_seconds: parse_metadata_f64(fields[26], "exposure time")?,
        aperture_f_number: parse_metadata_f64(fields[27], "aperture")?,
        focal_length_mm: parse_metadata_f64(fields[28], "focal length")?,
        captured_at_unix_seconds: parse_metadata_i64(fields[29], "capture time")?,
        lens_make: decode_text(30, "lens make")?,
        lens_model: decode_text(31, "lens model")?,
        focal_length_35mm: parse_metadata_f64(fields[32], "35 mm focal length")?,
    })
}

fn parse_decoder_snapshot_protocol(
    stdout: &[u8],
    expected_nonce: &str,
) -> Result<IsolatedPhotoDecoderSnapshot> {
    if expected_nonce.is_empty() {
        bail!("isolated decoder snapshot nonce must be non-empty");
    }
    let response =
        std::str::from_utf8(stdout).context("decode isolated RAW decoder snapshot output")?;
    let fields = response.split_whitespace().collect::<Vec<_>>();
    if fields.len() < DECODER_SNAPSHOT_BASE_FIELD_COUNT
        || fields[0] != DECODER_SNAPSHOT_PROTOCOL
        || fields[1] != "decoder-snapshot"
        || fields[2] != expected_nonce
    {
        bail!("isolated RAW decoder snapshot returned an invalid protocol response");
    }

    let preview_count = usize::try_from(parse_metadata_u64(fields[57], "preview count")?)
        .context("isolated RAW decoder snapshot preview count exceeds usize")?;
    if preview_count > MAX_DECODER_SNAPSHOT_PREVIEWS {
        bail!("isolated RAW decoder snapshot exceeded the preview descriptor limit");
    }
    let expected_field_count = DECODER_SNAPSHOT_BASE_FIELD_COUNT
        .checked_add(
            preview_count
                .checked_mul(DECODER_SNAPSHOT_PREVIEW_FIELD_COUNT)
                .ok_or_else(|| {
                    anyhow::anyhow!("isolated RAW decoder snapshot preview count overflow")
                })?,
        )
        .ok_or_else(|| anyhow::anyhow!("isolated RAW decoder snapshot field count overflow"))?;
    if fields.len() != expected_field_count {
        bail!("isolated RAW decoder snapshot returned an invalid field count");
    }

    let router_provider_id = decode_protocol_hex_text_field(
        fields[3],
        "decoder snapshot router provider id",
        MAX_DECODER_SNAPSHOT_IDENTITY_TEXT_BYTES,
        false,
    )?;
    let router_provider_version = decode_protocol_hex_text_field(
        fields[4],
        "decoder snapshot router provider version",
        MAX_DECODER_SNAPSHOT_IDENTITY_TEXT_BYTES,
        false,
    )?;
    let provider = DecodeProviderSnapshot {
        id: router_provider_id.clone(),
        version: router_provider_version.clone(),
        dng_sdk: parse_decoder_snapshot_bool(fields[5], "DNG SDK support")?,
        rawspeed: parse_decoder_snapshot_bool(fields[6], "RawSpeed support")?,
        jpeg: parse_decoder_snapshot_bool(fields[7], "JPEG support")?,
    };
    let metadata = parse_raw_metadata_snapshot_fields(
        &fields[8..41],
        MAX_DECODER_SNAPSHOT_METADATA_TEXT_BYTES,
        "isolated RAW decoder snapshot",
    )?;
    let capabilities = parse_decoder_snapshot_capabilities(&fields[41..57])?;
    let mut previews = Vec::with_capacity(preview_count);
    for index in 0..preview_count {
        let start =
            DECODER_SNAPSHOT_BASE_FIELD_COUNT + index * DECODER_SNAPSHOT_PREVIEW_FIELD_COUNT;
        previews.push(parse_decoder_snapshot_preview(&fields[start..start + 8])?);
    }

    Ok(IsolatedPhotoDecoderSnapshot {
        router_provider_id,
        router_provider_version,
        snapshot: DecoderSnapshot {
            provider,
            metadata,
            capabilities,
            previews,
        },
    })
}

fn parse_decoder_snapshot_capabilities(fields: &[&str]) -> Result<DecodeCapabilitySnapshot> {
    if fields.len() != DECODER_SNAPSHOT_CAPABILITY_FIELD_COUNT {
        bail!("isolated RAW decoder snapshot returned invalid capabilities");
    }
    Ok(DecodeCapabilitySnapshot {
        metadata: parse_decoder_snapshot_support(fields[0], "metadata capability")?,
        embedded_previews: parse_decoder_snapshot_support(
            fields[1],
            "embedded-preview capability",
        )?,
        raw_frame: parse_decoder_snapshot_support(fields[2], "raw-frame capability")?,
        reference_rgb: parse_decoder_snapshot_support(fields[3], "reference-rgb capability")?,
        pending_corrections: PendingCorrectionsSnapshot {
            dng_opcode_list_bytes: [
                parse_metadata_u32(fields[4], "DNG opcode list 1 bytes")?,
                parse_metadata_u32(fields[5], "DNG opcode list 2 bytes")?,
                parse_metadata_u32(fields[6], "DNG opcode list 3 bytes")?,
            ],
        },
        raw_development: RawDevelopmentCapabilitySnapshot {
            plan_schema_version: parse_metadata_u32(fields[7], "RAW development schema")?,
            available: parse_decoder_snapshot_support(fields[8], "RAW development availability")?,
            raw_frame: parse_decoder_snapshot_support(fields[9], "RAW development raw-frame")?,
            dng_opcode_execution_receipt: parse_decoder_snapshot_support(
                fields[10],
                "RAW development opcode receipt",
            )?,
            supported_intents: parse_metadata_u32(fields[11], "RAW development intents")?,
            supported_qualities: parse_metadata_u32(fields[12], "RAW development qualities")?,
            supported_dng_opcode_policies: parse_metadata_u32(
                fields[13],
                "RAW development opcode policies",
            )?,
            supported_noise_reduction_intents: parse_metadata_u32(
                fields[14],
                "RAW development denoise intents",
            )?,
            supported_highlight_recovery_intents: parse_metadata_u32(
                fields[15],
                "RAW development highlight intents",
            )?,
        },
    })
}

fn parse_decoder_snapshot_preview(fields: &[&str]) -> Result<PreviewDescriptorSnapshot> {
    if fields.len() != DECODER_SNAPSHOT_PREVIEW_FIELD_COUNT {
        bail!("isolated RAW decoder snapshot returned an invalid preview descriptor");
    }
    let provider_id = usize::try_from(parse_metadata_u64(fields[0], "preview provider id")?)
        .context("isolated RAW decoder snapshot preview provider id exceeds usize")?;
    let codec = match parse_metadata_u64(fields[1], "preview codec")? {
        0 => PreviewCodec::Unknown,
        1 => PreviewCodec::Jpeg,
        2 => PreviewCodec::Bitmap,
        3 => PreviewCodec::JpegXl,
        4 => PreviewCodec::H265,
        _ => bail!("isolated RAW decoder snapshot returned an unknown preview codec"),
    };
    Ok(PreviewDescriptorSnapshot {
        provider_id,
        codec,
        dimensions: ImageDimensions {
            width: parse_metadata_u32(fields[2], "preview width")?,
            height: parse_metadata_u32(fields[3], "preview height")?,
        },
        bits_per_channel: u16::try_from(parse_metadata_u64(fields[4], "preview bits")?)
            .context("isolated RAW decoder snapshot preview bits exceed u16")?,
        channels: u16::try_from(parse_metadata_u64(fields[5], "preview channels")?)
            .context("isolated RAW decoder snapshot preview channels exceed u16")?,
        encoded_bytes: parse_metadata_u64(fields[6], "preview encoded bytes")?,
        decodable: parse_decoder_snapshot_bool(fields[7], "preview decodable")?,
    })
}

fn parse_decoder_snapshot_support(encoded: &str, label: &str) -> Result<DecodeSupport> {
    Ok(if parse_decoder_snapshot_bool(encoded, label)? {
        DecodeSupport::Available
    } else {
        DecodeSupport::Unavailable
    })
}

fn parse_decoder_snapshot_bool(encoded: &str, label: &str) -> Result<bool> {
    match parse_metadata_u64(encoded, label)? {
        0 => Ok(false),
        1 => Ok(true),
        _ => bail!("isolated RAW decoder snapshot {label} is not a boolean"),
    }
}

fn decode_receipt_identity_field(encoded: &str, label: &str) -> Result<String> {
    let decoded = decode_protocol_hex_text_field(encoded, label, MAX_RECEIPT_TEXT_BYTES, false)?;
    // Pipeline identities can carry a human-readable fallback reason, which
    // legitimately contains spaces. Newlines, tabs, and other controls remain
    // forbidden even though the child encoded the field as hex.
    if !decoded
        .bytes()
        .all(|byte| byte == b' ' || byte.is_ascii_graphic())
    {
        bail!("isolated preview-development {label} is not a printable identity");
    }
    Ok(decoded)
}

fn decode_protocol_hex_text_field(
    encoded: &str,
    label: &str,
    maximum_bytes: usize,
    allow_empty: bool,
) -> Result<String> {
    if encoded == "-" {
        if allow_empty {
            return Ok(String::new());
        }
        bail!("isolated helper {label} must not be empty");
    }
    if encoded.is_empty() || encoded.len() % 2 != 0 || encoded.len() / 2 > maximum_bytes {
        bail!("isolated helper {label} is not a bounded hex value");
    }
    let mut bytes = Vec::with_capacity(encoded.len() / 2);
    for pair in encoded.as_bytes().chunks_exact(2) {
        let Some(high) = receipt_hex_nibble(pair[0]) else {
            bail!("isolated helper {label} contains invalid hex");
        };
        let Some(low) = receipt_hex_nibble(pair[1]) else {
            bail!("isolated helper {label} contains invalid hex");
        };
        bytes.push((high << 4) | low);
    }
    let decoded =
        String::from_utf8(bytes).with_context(|| format!("decode isolated helper {label}"))?;
    if (!allow_empty && decoded.is_empty()) || decoded.chars().any(char::is_control) {
        bail!("isolated helper {label} contains invalid text");
    }
    Ok(decoded)
}

fn parse_metadata_u64(encoded: &str, label: &str) -> Result<u64> {
    if encoded.len() != 16 {
        bail!("isolated RAW metadata {label} is not a fixed-width integer");
    }
    let mut value = 0_u64;
    for byte in encoded.bytes() {
        let Some(nibble) = receipt_hex_nibble(byte) else {
            bail!("isolated RAW metadata {label} contains invalid hex");
        };
        value = (value << 4) | u64::from(nibble);
    }
    Ok(value)
}

fn parse_metadata_u32(encoded: &str, label: &str) -> Result<u32> {
    u32::try_from(parse_metadata_u64(encoded, label)?)
        .with_context(|| format!("isolated RAW metadata {label} exceeds u32"))
}

fn parse_metadata_i32(encoded: &str, label: &str) -> Result<i32> {
    Ok(parse_metadata_u32(encoded, label)? as i32)
}

fn parse_metadata_i64(encoded: &str, label: &str) -> Result<i64> {
    Ok(parse_metadata_u64(encoded, label)? as i64)
}

fn parse_metadata_f64(encoded: &str, label: &str) -> Result<f64> {
    let value = f64::from_bits(parse_metadata_u64(encoded, label)?);
    if !value.is_finite() {
        bail!("isolated RAW metadata {label} is not finite");
    }
    Ok(value)
}

fn receipt_hex_nibble(byte: u8) -> Option<u8> {
    match byte {
        b'0'..=b'9' => Some(byte - b'0'),
        b'a'..=b'f' => Some(byte - b'a' + 10),
        b'A'..=b'F' => Some(byte - b'A' + 10),
        _ => None,
    }
}

fn parse_helper_dimensions(stdout: &[u8]) -> Result<ImageDimensions> {
    let stdout = std::str::from_utf8(stdout).context("decode isolated RAW helper output")?;
    let fields = stdout.split_whitespace().collect::<Vec<_>>();
    if fields.len() != 5 || fields[0] != HELPER_PROTOCOL || fields[3] != "8" || fields[4] != "3" {
        bail!("isolated RAW decoder returned an invalid protocol response");
    }
    let width = fields[1]
        .parse::<u32>()
        .context("parse isolated RAW proxy width")?;
    let height = fields[2]
        .parse::<u32>()
        .context("parse isolated RAW proxy height")?;
    Ok(ImageDimensions { width, height })
}

#[cfg(test)]
mod tests {
    use std::{fs, path::PathBuf};

    #[cfg(unix)]
    use std::os::unix::fs::PermissionsExt;

    use super::*;

    fn safety_fixture(name: &str) -> (PathBuf, PathBuf, PathBuf) {
        let root = std::env::temp_dir().join(format!(
            "shadow-isolated-proxy-{name}-{}-{}",
            std::process::id(),
            Uuid::now_v7()
        ));
        fs::create_dir_all(&root).expect("create safety fixture root");
        let source = root.join("sample.raw");
        let helper = root.join("helper-binary");
        fs::write(&source, b"first source bytes").expect("write source fixture");
        fs::write(&helper, b"first helper bytes").expect("write helper fixture");
        (root, source, helper)
    }

    fn metadata_snapshot_protocol(nonce: &str) -> Vec<u8> {
        const ZERO: &str = "0000000000000000";
        const ONE: &str = "0000000000000001";
        let mut fields = vec![
            METADATA_SNAPSHOT_PROTOCOL,
            "metadata-snapshot",
            nonce,
            "736861646f772d726f75746572", // shadow-router
            "7631",                       // v1
            "4e696b6f6e",                 // Nikon
            "5a2039",                     // Z 9
            "4e696b6f6e",                 // Nikon
            "5a2039",                     // Z 9
            "-",                          // absent DNG version
        ];
        fields.extend([
            ONE,                // raw count
            "0000000000002040", // raw width
            "0000000000001580", // raw height
            "0000000000002040", // image width
            "0000000000001580", // image height
            ZERO,
            ZERO,
            ZERO,
            ZERO,
            ONE,        // orientation
            "52474742", // RGGB
            "0000000000000003",
            "000000000000000e",
            "0000000000000200",
            "0000000000003fff",
            "3ff0000000000000",
            "3ff0000000000000",
            "3ff0000000000000",
            "3ff0000000000000",
            ZERO,
            "4059000000000000", // ISO 100
            "3f80624dd2f1a9fc", // 1/125s
            "4016666666666666", // f/5.6
            "4038000000000000", // 24mm
            "0000000065c91400",
            "4e494b4f4e",                         // NIKON
            "4e494b4b4f52205a2032342d3132306d6d", // NIKKOR Z 24-120mm
            "4042000000000000",                   // 36mm equivalent
        ]);
        assert_eq!(fields.len(), METADATA_SNAPSHOT_FIELD_COUNT);
        fields.join(" ").into_bytes()
    }

    fn decoder_snapshot_protocol(nonce: &str) -> Vec<u8> {
        const ZERO: &str = "0000000000000000";
        const ONE: &str = "0000000000000001";
        let metadata_fields = String::from_utf8(metadata_snapshot_protocol(nonce))
            .expect("metadata fixture is UTF-8")
            .split_whitespace()
            .skip(5)
            .map(ToOwned::to_owned)
            .collect::<Vec<_>>();
        assert_eq!(metadata_fields.len(), RAW_METADATA_SNAPSHOT_FIELD_COUNT);

        let mut fields = vec![
            DECODER_SNAPSHOT_PROTOCOL.to_owned(),
            "decoder-snapshot".to_owned(),
            nonce.to_owned(),
            "736861646f772d68656c706572".to_owned(), // shadow-helper
            "76332d70726976617465".to_owned(),       // v3-private
            ONE.to_owned(),
            ZERO.to_owned(),
            ONE.to_owned(),
        ];
        fields.extend(metadata_fields);
        fields.extend([
            ONE.to_owned(),                // metadata
            ONE.to_owned(),                // embedded previews
            ONE.to_owned(),                // raw frame
            ONE.to_owned(),                // reference RGB
            ZERO.to_owned(),               // opcode list 1
            ZERO.to_owned(),               // opcode list 2
            ZERO.to_owned(),               // opcode list 3
            ONE.to_owned(),                // development schema
            ONE.to_owned(),                // development available
            ONE.to_owned(),                // development raw frame
            ZERO.to_owned(),               // opcode receipt
            "0000000000000007".to_owned(), // intents
            "0000000000000006".to_owned(), // qualities
            ONE.to_owned(),                // opcode policies
            ONE.to_owned(),                // denoise intents
            "0000000000000003".to_owned(), // highlight intents
            "0000000000000002".to_owned(), // preview count
            // JPEG preview descriptor.
            ZERO.to_owned(),
            ONE.to_owned(),
            "0000000000000fa0".to_owned(),
            "0000000000000bb8".to_owned(),
            "0000000000000008".to_owned(),
            "0000000000000003".to_owned(),
            "0000000000123456".to_owned(),
            ONE.to_owned(),
            // A second, unavailable/unknown descriptor must still round-trip
            // as a descriptor; no preview bytes cross the helper boundary.
            "0000000000000005".to_owned(),
            ZERO.to_owned(),
            ZERO.to_owned(),
            ZERO.to_owned(),
            ZERO.to_owned(),
            ZERO.to_owned(),
            ZERO.to_owned(),
            ZERO.to_owned(),
        ]);
        assert_eq!(
            fields.len(),
            DECODER_SNAPSHOT_BASE_FIELD_COUNT + 2 * DECODER_SNAPSHOT_PREVIEW_FIELD_COUNT
        );
        fields.join(" ").into_bytes()
    }

    #[cfg(unix)]
    fn write_decoder_snapshot_helper(path: &Path) {
        let mut fields = String::from_utf8(decoder_snapshot_protocol("placeholder-nonce"))
            .expect("decoder fixture is UTF-8")
            .split_whitespace()
            .map(ToOwned::to_owned)
            .collect::<Vec<_>>();
        // The helper receives `decoder-snapshot <source> <nonce>`; let the
        // tiny shell fixture echo the caller's nonce so the production parser
        // validates the full child command/wiring rather than only a fixture.
        fields[2] = "$3".to_owned();
        let script = format!("#!/bin/sh\nprintf '%s\\n' \"{}\"\n", fields.join(" "));
        fs::write(path, script).expect("write fake decoder snapshot helper");
        let mut permissions = fs::metadata(path)
            .expect("read fake helper metadata")
            .permissions();
        permissions.set_mode(0o700);
        fs::set_permissions(path, permissions).expect("make fake helper executable");
    }

    #[cfg(unix)]
    fn write_pipe_holding_helper(path: &Path) {
        // `sleep` inherits both pipes. The shell is the direct child and waits
        // for it, so a timeout must kill the helper's *process group* or the
        // desktop would remain stuck draining a pipe after the shell dies.
        fs::write(path, "#!/bin/sh\nsleep 2 &\nwait\n").expect("write pipe-holding fake helper");
        let mut permissions = fs::metadata(path)
            .expect("read pipe-holding helper metadata")
            .permissions();
        permissions.set_mode(0o700);
        fs::set_permissions(path, permissions).expect("make pipe-holding helper executable");
    }

    #[test]
    fn parses_the_tiny_helper_metadata_protocol() {
        assert_eq!(
            parse_helper_dimensions(b"shadow-proxy-v1 1200 800 8 3\n").expect("parse protocol"),
            ImageDimensions {
                width: 1200,
                height: 800,
            }
        );
    }

    #[test]
    fn rejects_a_protocol_with_unexpected_pixel_layout() {
        assert!(parse_helper_dimensions(b"shadow-proxy-v1 1200 800 16 3\n").is_err());
    }

    #[test]
    fn parses_only_the_explicit_metadata_probe_scope() {
        parse_probe_protocol(b"shadow-probe-v1 open-metadata\n").expect("parse probe protocol");
        assert!(parse_probe_protocol(b"shadow-probe-v1 raw-frame\n").is_err());
    }

    #[test]
    fn parses_a_nonce_bound_child_metadata_snapshot() {
        let snapshot = parse_metadata_snapshot_protocol(
            &metadata_snapshot_protocol("metadata-nonce"),
            "metadata-nonce",
        )
        .expect("parse metadata snapshot");
        assert_eq!(snapshot.router_provider_id, "shadow-router");
        assert_eq!(snapshot.router_provider_version, "v1");
        assert_eq!(snapshot.metadata.make, "Nikon");
        assert_eq!(snapshot.metadata.model, "Z 9");
        assert_eq!(snapshot.metadata.dng_version, None);
        assert_eq!(snapshot.metadata.raw_dimensions.width, 8256);
        assert_eq!(snapshot.metadata.raw_dimensions.height, 5504);
        assert_eq!(snapshot.metadata.orientation, 1);
        assert_eq!(snapshot.metadata.cfa_pattern, "RGGB");
        assert_eq!(snapshot.metadata.lens_model, "NIKKOR Z 24-120mm");
        assert_eq!(snapshot.metadata.iso_speed, 100.0);
        assert_eq!(snapshot.metadata.focal_length_35mm, 36.0);
    }

    #[test]
    fn rejects_stale_nonce_and_nonfinite_child_metadata() {
        let response = metadata_snapshot_protocol("metadata-nonce");
        assert!(parse_metadata_snapshot_protocol(&response, "other-nonce").is_err());

        let invalid = String::from_utf8(response)
            .expect("metadata test protocol is UTF-8")
            .replacen("3ff0000000000000", "7ff8000000000000", 1);
        assert!(parse_metadata_snapshot_protocol(invalid.as_bytes(), "metadata-nonce").is_err());
    }

    #[test]
    fn parses_and_normalizes_a_nonce_bound_child_decoder_snapshot() {
        let helper_snapshot = parse_decoder_snapshot_protocol(
            &decoder_snapshot_protocol("decoder-nonce"),
            "decoder-nonce",
        )
        .expect("parse decoder snapshot");
        assert_eq!(helper_snapshot.router_provider_id, "shadow-helper");
        assert_eq!(helper_snapshot.router_provider_version, "v3-private");
        assert_eq!(helper_snapshot.snapshot.provider.id, "shadow-helper");
        assert!(
            helper_snapshot
                .snapshot
                .capabilities
                .raw_frame
                .is_available()
        );
        assert!(
            helper_snapshot
                .snapshot
                .capabilities
                .raw_development
                .available
                .is_available()
        );
        assert_eq!(helper_snapshot.snapshot.previews.len(), 2);
        assert_eq!(
            helper_snapshot.snapshot.previews[0].codec,
            PreviewCodec::Jpeg
        );
        assert_eq!(helper_snapshot.snapshot.previews[0].dimensions.width, 4000);

        let catalog_snapshot = helper_snapshot
            .clone()
            .into_catalog_snapshot("shadow-photo-router", "host-public-v1");
        assert_eq!(catalog_snapshot.provider.id, "shadow-photo-router");
        assert_eq!(catalog_snapshot.provider.version, "host-public-v1");
        assert!(!catalog_snapshot.provider.dng_sdk);
        assert!(!catalog_snapshot.provider.rawspeed);
        assert!(!catalog_snapshot.provider.jpeg);
        assert_eq!(
            catalog_snapshot.capabilities.metadata,
            DecodeSupport::Available,
            "catalog projection can use copied metadata without reopening the RAW"
        );
        assert_eq!(
            catalog_snapshot.capabilities.embedded_previews,
            DecodeSupport::Unavailable,
            "the host cannot safely extract helper-side embedded preview bytes"
        );
        assert_eq!(
            catalog_snapshot.capabilities.raw_frame,
            DecodeSupport::Unavailable,
            "helper-only RawFrame access must not become a desktop capability"
        );
        assert_eq!(
            catalog_snapshot.capabilities.reference_rgb,
            DecodeSupport::Unavailable,
            "helper-only RGB rendering must not become a desktop capability"
        );
        assert_eq!(
            catalog_snapshot.capabilities.raw_development,
            RawDevelopmentCapabilitySnapshot::default(),
            "a catalog-only descriptor snapshot cannot negotiate host RAW development"
        );
        assert!(catalog_snapshot.previews.is_empty());
    }

    #[test]
    fn decoder_safety_key_samples_contents_beyond_size_and_timestamp() {
        let (root, source, helper) = safety_fixture("content-sample");
        fs::write(&source, b"aaaaaaaaaaaaaaaa").expect("write first same-size source");
        let first = DecoderSafetyKey::decoder_snapshot(&source, &helper)
            .expect("build first decoder safety key");
        fs::write(&source, b"bbbbbbbbbbbbbbbb").expect("replace same-size source");
        let second = DecoderSafetyKey::decoder_snapshot(&source, &helper)
            .expect("build second decoder safety key");
        assert_ne!(
            first.source_content_sample, second.source_content_sample,
            "same-size content replacement must change the helper cache identity"
        );
        assert_ne!(first, second);
        fs::remove_dir_all(root).expect("remove content-sample fixture");
    }

    #[test]
    fn helper_implementation_identity_uses_contents_not_only_stat_metadata() {
        let (root, _source, helper) = safety_fixture("helper-content-identity");
        fs::write(&helper, b"aaaaaaaaaaaaaaaa").expect("write first same-size helper");
        let first = isolated_helper_implementation_identity(Some(&helper))
            .expect("build first helper implementation identity");
        fs::write(&helper, b"bbbbbbbbbbbbbbbb").expect("replace same-size helper");
        let second = isolated_helper_implementation_identity(Some(&helper))
            .expect("build second helper implementation identity");
        assert_ne!(
            first, second,
            "a helper replacement must invalidate the catalog-facing helper graph identity"
        );
        fs::remove_dir_all(root).expect("remove helper-content identity fixture");
    }

    #[cfg(unix)]
    #[test]
    fn helper_timeout_kills_the_process_group_before_pipe_draining_can_block() {
        let (root, _source, helper) = safety_fixture("process-group-timeout");
        write_pipe_holding_helper(&helper);
        let started_at = Instant::now();
        let result = execute_decode_helper_with_timeout(&helper, Duration::from_millis(80), |_| {})
            .expect("execute pipe-holding helper");
        assert!(matches!(result, HelperExecution::TimedOut(_)));
        assert!(
            started_at.elapsed() < Duration::from_secs(1),
            "timeout must not wait for a descendant retaining stdout/stderr"
        );
        fs::remove_dir_all(root).expect("remove process-group timeout fixture");
    }

    #[test]
    fn rejects_stale_or_invalid_child_decoder_snapshots() {
        let response = decoder_snapshot_protocol("decoder-nonce");
        assert!(parse_decoder_snapshot_protocol(&response, "other-nonce").is_err());

        let mut fields = String::from_utf8(response)
            .expect("decoder fixture is UTF-8")
            .split_whitespace()
            .map(ToOwned::to_owned)
            .collect::<Vec<_>>();
        fields[DECODER_SNAPSHOT_BASE_FIELD_COUNT + 1] = "0000000000000005".to_owned();
        assert!(
            parse_decoder_snapshot_protocol(fields.join(" ").as_bytes(), "decoder-nonce").is_err()
        );

        let mut impossible_count = String::from_utf8(decoder_snapshot_protocol("decoder-nonce"))
            .expect("decoder fixture is UTF-8")
            .split_whitespace()
            .map(ToOwned::to_owned)
            .collect::<Vec<_>>();
        impossible_count[57] = "0000000000000041".to_owned();
        assert!(
            parse_decoder_snapshot_protocol(impossible_count.join(" ").as_bytes(), "decoder-nonce")
                .is_err()
        );
    }

    #[test]
    fn parses_a_nonce_bound_preview_development_receipt() {
        let receipt = parse_preview_development_protocol(
            b"shadow-probe-v2 preview-development current-nonce 2 6c6962726177 302e32312e30 706970656c696e652d6964656e74697479 7265717565737465642d706c616e 6566666563746976652d706c616e\n",
            "current-nonce",
        )
        .expect("parse preview-development receipt");
        assert_eq!(receipt.raw_pipeline_path, 2);
        assert_eq!(receipt.source_provider_id, "libraw");
        assert_eq!(receipt.source_provider_version, "0.21.0");
        assert_eq!(receipt.pipeline_receipt_identity, "pipeline-identity");
        assert_eq!(receipt.requested_plan_identity, "requested-plan");
        assert_eq!(receipt.effective_plan_identity, "effective-plan");
    }

    #[test]
    fn rejects_wrong_nonce_or_malformed_preview_development_receipts() {
        let response = b"shadow-probe-v2 preview-development current-nonce 2 6c6962726177 302e32312e30 706970656c696e652d6964656e74697479 7265717565737465642d706c616e 6566666563746976652d706c616e\n";
        assert!(parse_preview_development_protocol(response, "other-nonce").is_err());
        assert!(parse_preview_development_protocol(
            b"shadow-probe-v2 preview-development current-nonce 2 zz 302e32312e30 706970656c696e652d6964656e74697479 7265717565737465642d706c616e 6566666563746976652d706c616e\n",
            "current-nonce",
        )
        .is_err());
    }

    #[test]
    fn preview_development_receipts_allow_a_printable_fallback_reason() {
        let receipt = parse_preview_development_protocol(
            b"shadow-probe-v2 preview-development current-nonce 2 6c6962726177 302e32312e30 66616c6c6261636b3d736f7572636520686173206e6f20726177206672616d65 7265717565737465642d706c616e 6566666563746976652d706c616e\n",
            "current-nonce",
        )
        .expect("parse fallback-bearing preview-development receipt");
        assert_eq!(
            receipt.pipeline_receipt_identity,
            "fallback=source has no raw frame"
        );
    }

    #[test]
    fn preview_development_receipts_are_keyed_and_persisted() {
        let (root, source, helper) = safety_fixture("preview-development-receipt");
        let key = DecoderSafetyKey::preview_development(&source, &helper, 1024)
            .expect("preview-development safety key");
        let receipt = IsolatedPreviewDevelopmentReceipt {
            raw_pipeline_path: 1,
            source_provider_id: "libraw".to_owned(),
            source_provider_version: "0.21.0".to_owned(),
            pipeline_receipt_identity: "pipeline-identity".to_owned(),
            requested_plan_identity: "requested-plan".to_owned(),
            effective_plan_identity: "effective-plan".to_owned(),
        };
        write_preview_development_receipt(&root, &key, &receipt)
            .expect("persist preview-development receipt");
        assert_eq!(
            read_preview_development_receipt(&root, &key)
                .expect("read preview-development receipt"),
            Some(receipt)
        );

        let different_edge = DecoderSafetyKey::preview_development(&source, &helper, 2048)
            .expect("different preview-development safety key");
        assert_eq!(
            read_preview_development_receipt(&root, &different_edge)
                .expect("read different preview-development receipt"),
            None,
            "a receipt for one preview scale cannot certify another one"
        );

        fs::remove_dir_all(root).expect("remove preview-development receipt fixture");
    }

    #[test]
    fn child_metadata_snapshots_are_keyed_and_persisted() {
        let (root, source, helper) = safety_fixture("metadata-snapshot");
        let key = DecoderSafetyKey::metadata_snapshot(&source, &helper)
            .expect("metadata snapshot safety key");
        let snapshot = parse_metadata_snapshot_protocol(
            &metadata_snapshot_protocol("metadata-nonce"),
            "metadata-nonce",
        )
        .expect("parse metadata snapshot fixture");
        write_metadata_snapshot(&root, &key, &snapshot).expect("persist metadata snapshot");
        assert_eq!(
            read_metadata_snapshot(&root, &key).expect("read metadata snapshot"),
            Some(snapshot)
        );

        fs::write(&source, b"changed source identity").expect("change metadata source fixture");
        let changed_key = DecoderSafetyKey::metadata_snapshot(&source, &helper)
            .expect("changed metadata snapshot safety key");
        assert_eq!(
            read_metadata_snapshot(&root, &changed_key).expect("read changed metadata snapshot"),
            None,
            "metadata from one source revision must not attach to another"
        );

        fs::remove_dir_all(root).expect("remove metadata snapshot fixture");
    }

    #[test]
    fn child_decoder_snapshots_are_keyed_and_persisted() {
        let (root, source, helper) = safety_fixture("decoder-snapshot");
        let key = DecoderSafetyKey::decoder_snapshot(&source, &helper)
            .expect("decoder snapshot safety key");
        let snapshot = parse_decoder_snapshot_protocol(
            &decoder_snapshot_protocol("decoder-nonce"),
            "decoder-nonce",
        )
        .expect("parse decoder snapshot fixture");
        write_decoder_snapshot(&root, &key, &snapshot).expect("persist decoder snapshot");
        assert_eq!(
            read_decoder_snapshot(&root, &key).expect("read decoder snapshot"),
            Some(snapshot)
        );

        fs::write(&helper, b"changed helper identity").expect("change decoder helper fixture");
        let changed_key = DecoderSafetyKey::decoder_snapshot(&source, &helper)
            .expect("changed decoder snapshot safety key");
        assert_eq!(
            read_decoder_snapshot(&root, &changed_key).expect("read changed decoder snapshot"),
            None,
            "a helper replacement must not reuse a prior child decoder snapshot"
        );

        fs::remove_dir_all(root).expect("remove decoder snapshot fixture");
    }

    #[cfg(unix)]
    #[test]
    fn child_decoder_snapshot_stage_executes_and_reuses_its_bounded_cache() {
        let (root, source, helper) = safety_fixture("decoder-snapshot-execution");
        write_decoder_snapshot_helper(&helper);
        let first = snapshot_isolated_photo_decoder(&helper, &root, &source)
            .expect("execute fake decoder snapshot helper");
        assert_eq!(first.router_provider_id, "shadow-helper");
        assert_eq!(first.snapshot.previews.len(), 2);

        // The second request must consume the immutable source/helper-keyed
        // record rather than launch another native helper process.
        let second = snapshot_isolated_photo_decoder(&helper, &root, &source)
            .expect("reuse cached decoder snapshot");
        assert_eq!(second, first);

        fs::write(&helper, b"this replacement is intentionally not executable")
            .expect("replace helper after cache write");
        let replacement_key =
            DecoderSafetyKey::decoder_snapshot(&source, &helper).expect("replacement helper key");
        assert!(
            read_decoder_snapshot(&root, &replacement_key)
                .expect("read replacement cache")
                .is_none(),
            "a changed helper cannot reuse an inspection snapshot"
        );

        fs::remove_dir_all(root).expect("remove decoder snapshot execution fixture");
    }

    #[test]
    fn legacy_proxy_success_never_promotes_native_raw_safety() {
        assert_eq!(
            native_decode_admission_from_observation(Some(
                IsolatedDecodeObservation::LegacyStageSucceededUnproven,
            )),
            NativeDecodeAdmission::NotQuarantined,
            "the legacy helper only proves bounded RGB proxy success"
        );
        assert_eq!(
            native_decode_admission_from_observation(
                Some(IsolatedDecodeObservation::ChildCrashed,)
            ),
            NativeDecodeAdmission::Quarantined {
                observation: IsolatedDecodeObservation::ChildCrashed,
            }
        );
    }

    #[test]
    fn child_crash_quarantine_persists_but_invalidates_when_source_changes() {
        let (root, source, helper) = safety_fixture("source-change");
        let key = DecoderSafetyKey::reference_proxy(&source, &helper).expect("fingerprint key");
        let metadata_probe =
            DecoderSafetyKey::metadata_probe(&source, &helper).expect("fingerprint probe key");
        assert_ne!(
            key, metadata_probe,
            "helper stages cannot share safety proof"
        );
        let first_process = DecoderSafetyRegistry::default();
        first_process
            .record(&root, &key, IsolatedDecodeObservation::ChildCrashed)
            .expect("persist child crash");

        let restarted_process = DecoderSafetyRegistry::default();
        assert_eq!(
            restarted_process
                .observation(&root, &key)
                .expect("read persisted crash"),
            Some(IsolatedDecodeObservation::ChildCrashed)
        );
        assert_eq!(
            native_decode_admission_from_observation(
                restarted_process
                    .observation(&root, &key)
                    .expect("read persisted crash again")
            ),
            NativeDecodeAdmission::Quarantined {
                observation: IsolatedDecodeObservation::ChildCrashed,
            }
        );

        fs::write(&source, b"a deliberately changed and longer source payload")
            .expect("change source fixture");
        let changed_key =
            DecoderSafetyKey::reference_proxy(&source, &helper).expect("fingerprint changed key");
        assert_ne!(key, changed_key);
        assert_eq!(
            restarted_process
                .observation(&root, &changed_key)
                .expect("read changed source observation"),
            None,
            "a crash quarantine must not follow a changed source fingerprint"
        );

        let decoder_snapshot_root = root.join("decoder-snapshot-crash");
        fs::create_dir_all(&decoder_snapshot_root).expect("create decoder snapshot safety root");
        let decoder_snapshot_registry = DecoderSafetyRegistry::default();
        let decoder_snapshot_key = DecoderSafetyKey::decoder_snapshot(&source, &helper)
            .expect("decoder snapshot safety key");
        decoder_snapshot_registry
            .record(
                &decoder_snapshot_root,
                &decoder_snapshot_key,
                IsolatedDecodeObservation::ChildCrashed,
            )
            .expect("persist decoder snapshot crash");
        assert_eq!(
            native_decode_admission_after_isolated_stages_for(
                &decoder_snapshot_registry,
                &decoder_snapshot_root,
                &source,
                &helper,
            )
            .expect("read decoder snapshot crash admission"),
            NativeDecodeAdmission::Quarantined {
                observation: IsolatedDecodeObservation::ChildCrashed,
            },
            "a full child inspection crash must block a later direct RawFrame session"
        );

        fs::remove_dir_all(root).expect("remove safety fixture");
    }

    #[test]
    fn aggregate_admission_fails_closed_for_either_isolated_stage() {
        let (root, source, helper) = safety_fixture("aggregate-stage-admission");
        let registry = DecoderSafetyRegistry::default();
        let metadata_key =
            DecoderSafetyKey::metadata_probe(&source, &helper).expect("metadata safety key");
        registry
            .record(
                &root,
                &metadata_key,
                IsolatedDecodeObservation::ChildCrashed,
            )
            .expect("persist metadata child crash");

        assert_eq!(
            native_decode_admission_after_isolated_stages_for(&registry, &root, &source, &helper)
                .expect("read aggregate safety admission"),
            NativeDecodeAdmission::Quarantined {
                observation: IsolatedDecodeObservation::ChildCrashed,
            },
            "a metadata-stage crash must block a later direct RawFrame session even if no proxy stage ran"
        );

        let timeout_root = root.join("proxy-timeout");
        fs::create_dir_all(&timeout_root).expect("create timeout safety root");
        let timeout_registry = DecoderSafetyRegistry::default();
        let proxy_key =
            DecoderSafetyKey::reference_proxy(&source, &helper).expect("proxy safety key");
        timeout_registry
            .record(
                &timeout_root,
                &proxy_key,
                IsolatedDecodeObservation::TimedOut,
            )
            .expect("persist proxy timeout");
        assert_eq!(
            native_decode_admission_after_isolated_stages_for(
                &timeout_registry,
                &timeout_root,
                &source,
                &helper,
            )
            .expect("read proxy timeout admission"),
            NativeDecodeAdmission::Quarantined {
                observation: IsolatedDecodeObservation::TimedOut,
            }
        );

        let snapshot_root = root.join("metadata-snapshot-crash");
        fs::create_dir_all(&snapshot_root).expect("create metadata snapshot safety root");
        let snapshot_registry = DecoderSafetyRegistry::default();
        let snapshot_key = DecoderSafetyKey::metadata_snapshot(&source, &helper)
            .expect("metadata snapshot safety key");
        snapshot_registry
            .record(
                &snapshot_root,
                &snapshot_key,
                IsolatedDecodeObservation::ChildCrashed,
            )
            .expect("persist metadata snapshot crash");
        assert_eq!(
            native_decode_admission_after_isolated_stages_for(
                &snapshot_registry,
                &snapshot_root,
                &source,
                &helper,
            )
            .expect("read metadata snapshot crash admission"),
            NativeDecodeAdmission::Quarantined {
                observation: IsolatedDecodeObservation::ChildCrashed,
            },
            "a metadata snapshot crash must block a later direct RawFrame session"
        );

        fs::remove_dir_all(root).expect("remove aggregate safety fixture");
    }
}
