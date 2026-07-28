use super::*;

fn plane(width: u32, height: u32, stride: u32, samples: &[f32]) -> DisplayLumaPlane<'_> {
    DisplayLumaPlane {
        contract_version: DISPLAY_LUMA_CONTRACT_VERSION,
        width,
        height,
        stride,
        samples,
        preprocessing_version: "display-luma-v1",
        input_source_hash: "blake3:fixture",
    }
}

#[test]
fn constant_plane_has_expected_distribution_and_zero_sharpness() {
    let observation = observe_display_luma(plane(3, 3, 3, &[0.25; 9])).expect("observe");
    assert_eq!(observation.metrics.histogram.bins().iter().sum::<u64>(), 9);
    assert_eq!(observation.metrics.histogram[64], 9);
    assert_close(observation.metrics.mean_luma.get(), 0.25);
    assert_close(observation.metrics.p01_luma.get(), 0.25);
    assert_close(observation.metrics.p50_luma.get(), 0.25);
    assert_close(observation.metrics.p99_luma.get(), 0.25);
    assert_eq!(observation.metrics.near_black_fraction, UnitInterval::ZERO);
    assert_eq!(observation.metrics.near_white_fraction, UnitInterval::ZERO);
    assert_close(observation.metrics.laplacian_variance.get(), 0.0);
    assert_close(observation.metrics.edge_energy.get(), 0.0);
}

#[test]
fn checkerboard_produces_positive_laplacian_and_edge_proxies() {
    let samples = [
        0.0, 1.0, 0.0, 1.0, 1.0, 0.0, 1.0, 0.0, 0.0, 1.0, 0.0, 1.0, 1.0, 0.0, 1.0, 0.0,
    ];
    let observation = observe_display_luma(plane(4, 4, 4, &samples)).expect("observe");
    assert_close(observation.metrics.laplacian_variance.get(), 16.0);
    assert_close(observation.metrics.edge_energy.get(), 1.0);
}

#[test]
fn row_padding_is_neither_validated_nor_measured() {
    let compact = [0.0, 1.0, 0.25, 0.75];
    let padded = [0.0, 1.0, f32::NAN, -10.0, 0.25, 0.75, f32::INFINITY, 10.0];
    let compact_observation = observe_display_luma(plane(2, 2, 2, &compact)).expect("compact");
    let padded_observation = observe_display_luma(plane(2, 2, 4, &padded)).expect("padded");
    assert_eq!(compact_observation.metrics, padded_observation.metrics);
    assert_eq!(padded_observation.input.stride, 4);
}

#[test]
fn nearest_rank_percentiles_and_histogram_boundaries_are_explicit() {
    let samples = (0_u16..100)
        .map(|value| f32::from(value) / 99.0)
        .collect::<Vec<_>>();
    let observation = observe_display_luma(plane(100, 1, 100, &samples)).expect("observe");
    assert_close(observation.metrics.p01_luma.get(), 0.0);
    assert_close(
        observation.metrics.p50_luma.get(),
        f64::from(49.0_f32 / 99.0),
    );
    assert_close(
        observation.metrics.p99_luma.get(),
        f64::from(98.0_f32 / 99.0),
    );

    let below_bin_one = f32::from_bits((1.0_f32 / 256.0).to_bits() - 1);
    let boundary = [0.0, below_bin_one, 1.0 / 256.0, 1.0];
    let histogram = observe_display_luma(plane(4, 1, 4, &boundary))
        .expect("observe boundaries")
        .metrics
        .histogram;
    assert_eq!(histogram[0], 2);
    assert_eq!(histogram[1], 1);
    assert_eq!(histogram[255], 1);
}

#[test]
fn near_black_and_white_thresholds_are_inclusive() {
    let samples = [0.0, 0.01, 0.02, 0.98, 0.99, 1.0];
    let metrics = observe_display_luma(plane(6, 1, 6, &samples))
        .expect("observe thresholds")
        .metrics;
    assert_close(metrics.near_black_fraction.get(), 2.0 / 6.0);
    assert_close(metrics.near_white_fraction.get(), 2.0 / 6.0);
}

#[test]
fn invalid_layout_values_and_versions_fail_closed() {
    assert!(matches!(
        observe_display_luma(plane(0, 1, 0, &[])),
        Err(TechnicalObservationError::ZeroDimension)
    ));
    assert!(matches!(
        observe_display_luma(plane(2, 1, 1, &[0.0])),
        Err(TechnicalObservationError::StrideTooSmall { .. })
    ));
    assert!(matches!(
        observe_display_luma(plane(2, 1, 2, &[0.0])),
        Err(TechnicalObservationError::BufferLengthMismatch { .. })
    ));
    for invalid in [f32::NAN, f32::INFINITY] {
        assert!(matches!(
            observe_display_luma(plane(1, 1, 1, &[invalid])),
            Err(TechnicalObservationError::NonFiniteLuma { .. })
        ));
    }
    for invalid in [-0.01, 1.01] {
        assert!(matches!(
            observe_display_luma(plane(1, 1, 1, &[invalid])),
            Err(TechnicalObservationError::LumaOutOfRange { .. })
        ));
    }
    let mut unknown = plane(1, 1, 1, &[0.5]);
    unknown.contract_version += 1;
    assert!(matches!(
        observe_display_luma(unknown),
        Err(TechnicalObservationError::UnsupportedDisplayLumaContract { .. })
    ));
    assert!(matches!(
        observe_display_luma(plane(MAX_DISPLAY_LUMA_DIMENSION + 1, 1, 1, &[])),
        Err(TechnicalObservationError::DimensionTooLarge { .. })
    ));
    assert!(matches!(
        observe_display_luma(plane(4_096, 4_097, 4_096, &[])),
        Err(TechnicalObservationError::TooManyActiveSamples { .. })
    ));
}

#[test]
fn observation_is_deterministic_and_serde_round_trips() {
    let samples = [0.0, 0.01, 0.2, 0.5, 0.8, 0.99, 1.0, 0.4, 0.6];
    let first = observe_display_luma(plane(3, 3, 3, &samples)).expect("first");
    let second = observe_display_luma(plane(3, 3, 3, &samples)).expect("second");
    assert_eq!(first, second);
    let first_json = serde_json::to_string(&first).expect("serialize first");
    let second_json = serde_json::to_string(&second).expect("serialize second");
    assert_eq!(first_json, second_json);
    let decoded: TechnicalQualityObservation =
        serde_json::from_str(&first_json).expect("deserialize observation");
    assert_eq!(decoded, first);
    assert_eq!(
        decoded.algorithm.schema_version(),
        TECHNICAL_QUALITY_SCHEMA_VERSION
    );
    assert_eq!(
        decoded.algorithm.implementation_version(),
        TECHNICAL_QUALITY_IMPLEMENTATION_VERSION
    );

    let mut unknown_schema = serde_json::to_value(&first).expect("serialize value");
    unknown_schema["algorithm"]["schema_version"] = serde_json::json!(2);
    assert!(
        serde_json::from_value::<TechnicalQualityObservation>(unknown_schema).is_err(),
        "unknown persisted algorithm schema must fail closed"
    );
    let mut unknown_implementation = serde_json::to_value(&first).expect("serialize value");
    unknown_implementation["algorithm"]["implementation_version"] = serde_json::json!("unknown-v2");
    assert!(
        serde_json::from_value::<TechnicalQualityObservation>(unknown_implementation).is_err(),
        "unknown persisted implementation must fail closed"
    );
    let mut unknown_contract = serde_json::to_value(&first).expect("serialize value");
    unknown_contract["input"]["display_luma_contract_version"] = serde_json::json!(2);
    assert!(
        serde_json::from_value::<TechnicalQualityObservation>(unknown_contract).is_err(),
        "unknown persisted luma contract must fail closed"
    );
}

fn assert_close(actual: f64, expected: f64) {
    assert!((actual - expected).abs() <= f64::EPSILON);
}
