//! Durable negative decoder evidence and advisory cache publication.
//!
//! A crash or timeout is persisted for one exact route identity and may
//! quarantine later in-process work. Other helper artifacts reuse the atomic
//! publication primitive, but retain their typed schemas in their own owners.

use std::{
    collections::HashMap,
    fs, io,
    path::{Path, PathBuf},
    sync::{Mutex, OnceLock},
};

use anyhow::{Context, Result};
use uuid::Uuid;

use super::route_identity::DecoderSafetyKey;

const SAFETY_RECORD_SCHEMA: u8 = 1;

/// What the isolated helper established about this specific source route.
///
/// A successful v1 proxy only means that a child process rendered one bounded
/// RGB proxy. It must never be mistaken for evidence that the host may safely
/// open a `RawFrame`, a full-detail session, or an export session in-process.
#[derive(Debug, Clone, Copy, Eq, PartialEq, serde::Serialize, serde::Deserialize)]
#[serde(rename_all = "snake_case")]
pub(crate) enum IsolatedDecodeObservation {
    /// The legacy helper completed only its requested bounded stage. Native
    /// `RawFrame`/detail/export safety is intentionally still unknown.
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
    pub(super) fn quarantines_main_process_native_decode(self) -> bool {
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

#[derive(Debug, Clone, Eq, PartialEq, serde::Serialize, serde::Deserialize)]
struct PersistedSafetyObservation {
    schema: u8,
    key: DecoderSafetyKey,
    observation: IsolatedDecodeObservation,
}

#[derive(Debug, Default)]
pub(super) struct DecoderSafetyRegistry {
    observations: Mutex<HashMap<DecoderSafetyKey, IsolatedDecodeObservation>>,
}

static DECODER_SAFETY_REGISTRY: OnceLock<DecoderSafetyRegistry> = OnceLock::new();

impl DecoderSafetyRegistry {
    pub(super) fn shared() -> &'static Self {
        DECODER_SAFETY_REGISTRY.get_or_init(Self::default)
    }

    pub(super) fn observation(
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

    pub(super) fn record(
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

fn safety_record_path(runtime_cache_root: &Path, key: &DecoderSafetyKey) -> Result<PathBuf> {
    let key = serde_json::to_vec(key).context("serialize isolated decoder safety key")?;
    let digest = blake3::hash(&key).to_hex();
    Ok(runtime_cache_root
        .join("decode-helper")
        .join("safety")
        .join(format!("{digest}.json")))
}

pub(super) fn write_atomic_cache_record(path: &Path, bytes: &[u8]) -> Result<()> {
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

pub(super) fn record_safety_observation_if_source_is_current(
    runtime_cache_root: &Path,
    source_path: &Path,
    helper_path: &Path,
    before: &DecoderSafetyKey,
    observation: IsolatedDecodeObservation,
) {
    if !before.is_current(source_path, helper_path) {
        return;
    }
    // A cache-record failure must never hide the original decoder diagnosis.
    // The in-memory registry is updated before the best-effort disk publish,
    // so this process still avoids a repeat child crash.
    let _ = DecoderSafetyRegistry::shared().record(runtime_cache_root, before, observation);
}

#[cfg(test)]
mod tests;
