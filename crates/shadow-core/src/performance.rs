//! Versioned, privacy-preserving import engine performance aggregates.

use std::time::Duration;

/// Schema version for [`ImportEnginePerformance`].
///
/// The profile contains only aggregate counts and durations. It deliberately
/// carries no paths, entity identifiers, provider diagnostics, or error text.
pub const IMPORT_ENGINE_PERFORMANCE_SCHEMA_VERSION: u32 = 1;

/// A fail-safe aggregate for one monotonic-duration phase.
#[derive(Debug, Clone, Copy, Default, Eq, PartialEq)]
pub struct DurationStats {
    pub samples: u64,
    pub total_ns: u64,
    pub max_ns: u64,
}

impl DurationStats {
    /// Adds one duration using saturating counters.
    pub fn record(&mut self, duration: Duration) {
        self.record_ns(u64::try_from(duration.as_nanos()).unwrap_or(u64::MAX));
    }

    /// Adds one already-normalized nanosecond duration using saturating
    /// counters.
    pub fn record_ns(&mut self, duration_ns: u64) {
        self.samples = self.samples.saturating_add(1);
        self.total_ns = self.total_ns.saturating_add(duration_ns);
        self.max_ns = self.max_ns.max(duration_ns);
    }
}

/// Scanner-thread aggregates for one explicitly profiled import session.
#[derive(Debug, Clone, Default, Eq, PartialEq)]
pub struct ScanPerformance {
    pub profiled: bool,
    pub discovery_io: DurationStats,
    pub metadata_stat: DurationStats,
    pub progress_callback: DurationStats,
    pub discovered_journal: DurationStats,
    pub asset_registration: DurationStats,
    pub decode_current_query: DurationStats,
    pub decode_submit_wait: DurationStats,
    pub decode_queue_full_events: u64,
    pub first_catalogued_ms: Option<u64>,
    pub enumeration_total_ms: u64,
}

/// Decode-worker aggregates for one explicitly profiled actor lifetime.
#[derive(Debug, Clone, Default, Eq, PartialEq)]
pub struct DecodePerformance {
    pub profiled: bool,
    pub queue_wait: DurationStats,
    pub source_guard_stat: DurationStats,
    pub provider_inspect: DurationStats,
    pub snapshot_catalog_commit: DurationStats,
    pub embedded_preview_extract: DurationStats,
    pub proxy_render: DurationStats,
    pub cache_blob_put: DurationStats,
    pub cache_artifact_catalog_commit: DurationStats,
    pub technical_submit_wait: DurationStats,
}

/// Technical-observation-worker aggregates for one explicitly profiled actor
/// lifetime.
#[derive(Debug, Clone, Default, Eq, PartialEq)]
pub struct TechnicalPerformance {
    pub profiled: bool,
    pub technical_observation_total: DurationStats,
}

/// Versioned terminal aggregate for one import engine run.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct ImportEnginePerformance {
    pub schema_version: u32,
    pub scan: ScanPerformance,
    pub decode: DecodePerformance,
    pub technical: TechnicalPerformance,
}

impl ImportEnginePerformance {
    #[must_use]
    pub const fn new(
        scan: ScanPerformance,
        decode: DecodePerformance,
        technical: TechnicalPerformance,
    ) -> Self {
        Self {
            schema_version: IMPORT_ENGINE_PERFORMANCE_SCHEMA_VERSION,
            scan,
            decode,
            technical,
        }
    }
}

impl Default for ImportEnginePerformance {
    fn default() -> Self {
        Self::new(
            ScanPerformance::default(),
            DecodePerformance::default(),
            TechnicalPerformance::default(),
        )
    }
}

pub(crate) fn measure_if<T>(
    enabled: bool,
    stats: &mut DurationStats,
    operation: impl FnOnce() -> T,
) -> T {
    if !enabled {
        return operation();
    }
    let started_at = std::time::Instant::now();
    let result = operation();
    stats.record(started_at.elapsed());
    result
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn duration_stats_saturate_without_losing_the_maximum() {
        let mut stats = DurationStats {
            samples: u64::MAX,
            total_ns: u64::MAX - 2,
            max_ns: 5,
        };
        stats.record_ns(10);

        assert_eq!(stats.samples, u64::MAX);
        assert_eq!(stats.total_ns, u64::MAX);
        assert_eq!(stats.max_ns, 10);
    }

    #[test]
    fn engine_profile_is_explicitly_versioned() {
        let profile = ImportEnginePerformance::default();
        assert_eq!(
            profile.schema_version,
            IMPORT_ENGINE_PERFORMANCE_SCHEMA_VERSION
        );
        assert!(!profile.scan.profiled);
        assert!(!profile.decode.profiled);
        assert!(!profile.technical.profiled);
    }
}
