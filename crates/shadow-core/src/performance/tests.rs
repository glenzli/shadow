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
