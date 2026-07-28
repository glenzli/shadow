//! Conservative native-decode admission derived from durable negative evidence.

use std::path::Path;

use anyhow::Result;

use super::{
    persistent_evidence::{DecoderSafetyRegistry, IsolatedDecodeObservation},
    route_identity::DecoderSafetyKey,
};

/// The only admission decision this safety layer is allowed to make.
///
/// `NotQuarantined` deliberately does *not* mean "native RAW safe". It means
/// only that this exact source/helper route has not previously crashed or
/// timed out in an isolated child.
#[derive(Debug, Clone, Copy, Eq, PartialEq)]
pub(crate) enum NativeDecodeAdmission {
    NotQuarantined,
    Quarantined {
        observation: IsolatedDecodeObservation,
    },
}

/// Combines the child stages that can establish a *negative* main-process
/// safety fact. A successful stage never promotes a source to native-safe;
/// only a crash or timeout makes the parent fail closed for this source
/// revision.
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
    // Keep this order and the legacy probe identity stable. Existing
    // crash/timeout records must remain effective even though the unreachable
    // metadata-probe executor itself has been removed.
    for key in [
        DecoderSafetyKey::legacy_metadata_probe(source_path, helper_path)?,
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

#[cfg(test)]
mod tests;
