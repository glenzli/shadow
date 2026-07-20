//! Deterministic, model-free observations from a normalized display-luma proxy.
//!
//! These metrics describe the supplied display-referred proxy. They are not RAW
//! exposure measurements, aesthetic judgments, Pick/Reject decisions, or a
//! calibrated technical-quality score.

use std::ops::Index;

use serde::{Deserialize, Serialize};
use thiserror::Error;

use crate::UnitInterval;

/// Semantic contract understood by this implementation.
pub const DISPLAY_LUMA_CONTRACT_VERSION: u32 = 1;
/// Persisted output schema for [`TechnicalQualityObservation`].
pub const TECHNICAL_QUALITY_SCHEMA_VERSION: u32 = 1;
/// Exact CPU implementation recorded in every observation.
pub const TECHNICAL_QUALITY_IMPLEMENTATION_VERSION: &str = "shadow.display-luma.cpu-reference-v1";
/// Fixed histogram size. Bin `i` covers `[i/256, (i+1)/256)`, except the last
/// bin also includes exactly `1.0`.
pub const LUMA_HISTOGRAM_BIN_COUNT: usize = 256;
/// Pixels at or below this display-luma value count as near black.
pub const NEAR_BLACK_LUMA_THRESHOLD: f32 = 0.01;
/// Pixels at or above this display-luma value count as near white.
pub const NEAR_WHITE_LUMA_THRESHOLD: f32 = 0.99;
/// Display proxies larger than this in either dimension are rejected.
pub const MAX_DISPLAY_LUMA_DIMENSION: u32 = 8_192;
/// Active pixels are bounded to keep exact percentile memory and runtime finite.
pub const MAX_ACTIVE_LUMA_SAMPLES: usize = 16_777_216;
/// Stride padding is allowed, but the complete supplied plane remains bounded.
pub const MAX_DISPLAY_LUMA_BUFFER_SAMPLES: usize = 33_554_432;

const MAX_PROVENANCE_FIELD_BYTES: usize = 256;

/// A borrowed, row-major plane of normalized display-referred luminance.
///
/// `stride` is measured in `f32` samples, not bytes. The slice must contain
/// exactly `stride * height` samples. Only columns `0..width` are interpreted;
/// row padding is deliberately neither validated nor measured.
#[derive(Debug, Copy, Clone)]
pub struct DisplayLumaPlane<'a> {
    pub contract_version: u32,
    pub width: u32,
    pub height: u32,
    pub stride: u32,
    pub samples: &'a [f32],
    /// Exact preprocessing pipeline revision that produced this display proxy.
    pub preprocessing_version: &'a str,
    /// Content identity of the input proxy, supplied by the caller.
    pub input_source_hash: &'a str,
}

/// Persisted identity of the measured plane; padding values are not represented.
#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(try_from = "TechnicalInputProvenanceWire")]
pub struct TechnicalInputProvenance {
    pub display_luma_contract_version: u32,
    pub width: u32,
    pub height: u32,
    pub stride: u32,
    pub preprocessing_version: String,
    pub input_source_hash: String,
}

#[derive(Debug, Deserialize)]
struct TechnicalInputProvenanceWire {
    display_luma_contract_version: u32,
    width: u32,
    height: u32,
    stride: u32,
    preprocessing_version: String,
    input_source_hash: String,
}

impl TryFrom<TechnicalInputProvenanceWire> for TechnicalInputProvenance {
    type Error = TechnicalObservationError;

    fn try_from(value: TechnicalInputProvenanceWire) -> Result<Self, Self::Error> {
        validate_provenance_layout(
            value.display_luma_contract_version,
            value.width,
            value.height,
            value.stride,
            &value.preprocessing_version,
            &value.input_source_hash,
        )?;
        Ok(Self {
            display_luma_contract_version: value.display_luma_contract_version,
            width: value.width,
            height: value.height,
            stride: value.stride,
            preprocessing_version: value.preprocessing_version,
            input_source_hash: value.input_source_hash,
        })
    }
}

/// Stable identity of the rule implementation, independent from any model.
#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(try_from = "TechnicalAlgorithmProvenanceWire")]
pub struct TechnicalAlgorithmProvenance {
    schema_version: u32,
    implementation_version: String,
}

#[derive(Debug, Deserialize)]
struct TechnicalAlgorithmProvenanceWire {
    schema_version: u32,
    implementation_version: String,
}

impl TechnicalAlgorithmProvenance {
    pub const fn schema_version(&self) -> u32 {
        self.schema_version
    }

    pub fn implementation_version(&self) -> &str {
        &self.implementation_version
    }
}

impl TryFrom<TechnicalAlgorithmProvenanceWire> for TechnicalAlgorithmProvenance {
    type Error = TechnicalObservationError;

    fn try_from(value: TechnicalAlgorithmProvenanceWire) -> Result<Self, Self::Error> {
        if value.schema_version != TECHNICAL_QUALITY_SCHEMA_VERSION {
            return Err(TechnicalObservationError::UnsupportedTechnicalSchema {
                provided: value.schema_version,
                supported: TECHNICAL_QUALITY_SCHEMA_VERSION,
            });
        }
        if value.implementation_version != TECHNICAL_QUALITY_IMPLEMENTATION_VERSION {
            return Err(
                TechnicalObservationError::UnsupportedTechnicalImplementation {
                    provided: value.implementation_version,
                    supported: TECHNICAL_QUALITY_IMPLEMENTATION_VERSION,
                },
            );
        }
        Ok(Self {
            schema_version: value.schema_version,
            implementation_version: value.implementation_version,
        })
    }
}

/// A finite, non-negative scalar suitable for durable observation payloads.
#[derive(Debug, Copy, Clone, PartialEq, PartialOrd, Serialize, Deserialize)]
#[serde(try_from = "f64", into = "f64")]
pub struct NonNegativeFinite(f64);

impl NonNegativeFinite {
    /// Creates a finite, non-negative value.
    ///
    /// # Errors
    ///
    /// Returns [`TechnicalObservationError::InvalidNonNegativeMetric`] for NaN,
    /// infinity, or a negative value.
    pub fn new(value: f64) -> Result<Self, TechnicalObservationError> {
        if value.is_finite() && value >= 0.0 {
            Ok(Self(value))
        } else {
            Err(TechnicalObservationError::InvalidNonNegativeMetric(value))
        }
    }

    pub const fn get(self) -> f64 {
        self.0
    }
}

impl TryFrom<f64> for NonNegativeFinite {
    type Error = TechnicalObservationError;

    fn try_from(value: f64) -> Result<Self, Self::Error> {
        Self::new(value)
    }
}

impl From<NonNegativeFinite> for f64 {
    fn from(value: NonNegativeFinite) -> Self {
        value.get()
    }
}

/// Explainable measurements, deliberately kept separate from ranking policy.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct TechnicalQualityMetrics {
    pub histogram: LumaHistogram,
    pub mean_luma: UnitInterval,
    /// Exact observed sample at nearest-rank percentile `ceil(0.01 * N)`.
    pub p01_luma: UnitInterval,
    /// Exact observed sample at nearest-rank percentile `ceil(0.50 * N)`.
    pub p50_luma: UnitInterval,
    /// Exact observed sample at nearest-rank percentile `ceil(0.99 * N)`.
    pub p99_luma: UnitInterval,
    pub near_black_fraction: UnitInterval,
    pub near_white_fraction: UnitInterval,
    /// Population variance of the 4-neighbor Laplacian over interior pixels.
    pub laplacian_variance: NonNegativeFinite,
    /// Mean squared difference across horizontal and vertical pixel neighbors.
    pub edge_energy: NonNegativeFinite,
}

/// Exactly 256 deterministic display-luma bin counts.
#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(try_from = "Vec<u64>", into = "Vec<u64>")]
pub struct LumaHistogram([u64; LUMA_HISTOGRAM_BIN_COUNT]);

impl LumaHistogram {
    pub const fn bins(&self) -> &[u64; LUMA_HISTOGRAM_BIN_COUNT] {
        &self.0
    }
}

impl TryFrom<Vec<u64>> for LumaHistogram {
    type Error = TechnicalObservationError;

    fn try_from(value: Vec<u64>) -> Result<Self, Self::Error> {
        let actual = value.len();
        value
            .try_into()
            .map(Self)
            .map_err(|_| TechnicalObservationError::InvalidHistogramLength {
                actual,
                expected: LUMA_HISTOGRAM_BIN_COUNT,
            })
    }
}

impl From<LumaHistogram> for Vec<u64> {
    fn from(value: LumaHistogram) -> Self {
        value.0.into()
    }
}

impl Index<usize> for LumaHistogram {
    type Output = u64;

    fn index(&self, index: usize) -> &Self::Output {
        &self.0[index]
    }
}

/// Typed technical payload. Generic request/generation/task fields continue to
/// belong to the existing `AiObservation` envelope rather than being duplicated.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct TechnicalQualityObservation {
    pub algorithm: TechnicalAlgorithmProvenance,
    pub input: TechnicalInputProvenance,
    pub metrics: TechnicalQualityMetrics,
}

#[derive(Debug, Clone, PartialEq, Error)]
pub enum TechnicalObservationError {
    #[error(
        "unsupported display-luma contract version {provided}; this build supports {supported}"
    )]
    UnsupportedDisplayLumaContract { provided: u32, supported: u32 },
    #[error("unsupported technical observation schema {provided}; supported schema is {supported}")]
    UnsupportedTechnicalSchema { provided: u32, supported: u32 },
    #[error(
        "unsupported technical implementation {provided:?}; supported implementation is {supported:?}"
    )]
    UnsupportedTechnicalImplementation {
        provided: String,
        supported: &'static str,
    },
    #[error("display-luma width and height must both be greater than zero")]
    ZeroDimension,
    #[error("display-luma {field} {value} exceeds the maximum {maximum}")]
    DimensionTooLarge {
        field: &'static str,
        value: u32,
        maximum: u32,
    },
    #[error("display-luma stride {stride} is smaller than width {width}")]
    StrideTooSmall { width: u32, stride: u32 },
    #[error("display-luma dimensions or stride overflow addressable memory")]
    SizeOverflow,
    #[error("active display-luma sample count {actual} exceeds maximum {maximum}")]
    TooManyActiveSamples { actual: usize, maximum: usize },
    #[error("display-luma buffer sample count {actual} exceeds maximum {maximum}")]
    BufferTooLarge { actual: usize, maximum: usize },
    #[error("display-luma buffer has {actual} samples; expected exactly {expected}")]
    BufferLengthMismatch { expected: usize, actual: usize },
    #[error("{field} must not be empty")]
    EmptyProvenance { field: &'static str },
    #[error("{field} exceeds {maximum} bytes")]
    ProvenanceTooLong { field: &'static str, maximum: usize },
    #[error("display-luma sample at ({x}, {y}) must be finite")]
    NonFiniteLuma { x: u32, y: u32 },
    #[error("display-luma sample at ({x}, {y}) is outside 0 through 1: {value}")]
    LumaOutOfRange { x: u32, y: u32, value: f32 },
    #[error("computed unit metric violated its finite 0 through 1 invariant")]
    InvalidComputedUnitMetric,
    #[error("histogram has {actual} bins; expected exactly {expected}")]
    InvalidHistogramLength { actual: usize, expected: usize },
    #[error("metric must be finite and non-negative, got {0}")]
    InvalidNonNegativeMetric(f64),
}

/// Computes deterministic observations from a normalized display-luma plane.
///
/// Exact percentiles use the nearest-rank rule and therefore require bounded
/// scratch storage. Padding never contributes to validation, histograms, or
/// sharpness proxies.
///
/// # Errors
///
/// Returns [`TechnicalObservationError`] for unknown contracts, invalid layout,
/// oversized inputs, malformed provenance, or invalid active luma samples.
pub fn observe_display_luma(
    plane: DisplayLumaPlane<'_>,
) -> Result<TechnicalQualityObservation, TechnicalObservationError> {
    let dimensions = validate_plane(&plane)?;
    let aggregates = collect_luma(&plane, dimensions)?;
    let (laplacian_variance, edge_energy) = sharpness_proxies(&plane, dimensions)?;
    let sample_count = u32::try_from(aggregates.sorted_luma.len())
        .map_err(|_| TechnicalObservationError::SizeOverflow)?;
    let denominator = f64::from(sample_count);
    let metrics = TechnicalQualityMetrics {
        histogram: LumaHistogram(aggregates.histogram),
        mean_luma: unit(aggregates.sum / denominator)?,
        p01_luma: unit(nearest_rank(&aggregates.sorted_luma, 1, 100))?,
        p50_luma: unit(nearest_rank(&aggregates.sorted_luma, 1, 2))?,
        p99_luma: unit(nearest_rank(&aggregates.sorted_luma, 99, 100))?,
        near_black_fraction: unit(f64::from(aggregates.near_black) / denominator)?,
        near_white_fraction: unit(f64::from(aggregates.near_white) / denominator)?,
        laplacian_variance: NonNegativeFinite::new(laplacian_variance)?,
        edge_energy: NonNegativeFinite::new(edge_energy)?,
    };
    Ok(TechnicalQualityObservation {
        algorithm: TechnicalAlgorithmProvenance {
            schema_version: TECHNICAL_QUALITY_SCHEMA_VERSION,
            implementation_version: TECHNICAL_QUALITY_IMPLEMENTATION_VERSION.into(),
        },
        input: TechnicalInputProvenance {
            display_luma_contract_version: plane.contract_version,
            width: plane.width,
            height: plane.height,
            stride: plane.stride,
            preprocessing_version: plane.preprocessing_version.into(),
            input_source_hash: plane.input_source_hash.into(),
        },
        metrics,
    })
}

#[derive(Debug, Copy, Clone)]
struct ValidDimensions {
    width: usize,
    height: usize,
    stride: usize,
}

struct LumaAggregates {
    histogram: [u64; LUMA_HISTOGRAM_BIN_COUNT],
    sorted_luma: Vec<f32>,
    sum: f64,
    near_black: u32,
    near_white: u32,
}

fn validate_plane(
    plane: &DisplayLumaPlane<'_>,
) -> Result<ValidDimensions, TechnicalObservationError> {
    let dimensions = validate_provenance_layout(
        plane.contract_version,
        plane.width,
        plane.height,
        plane.stride,
        plane.preprocessing_version,
        plane.input_source_hash,
    )?;
    let expected = dimensions
        .stride
        .checked_mul(dimensions.height)
        .ok_or(TechnicalObservationError::SizeOverflow)?;
    if plane.samples.len() != expected {
        return Err(TechnicalObservationError::BufferLengthMismatch {
            expected,
            actual: plane.samples.len(),
        });
    }
    Ok(dimensions)
}

fn validate_provenance_layout(
    contract_version: u32,
    width: u32,
    height: u32,
    stride: u32,
    preprocessing_version: &str,
    input_source_hash: &str,
) -> Result<ValidDimensions, TechnicalObservationError> {
    if contract_version != DISPLAY_LUMA_CONTRACT_VERSION {
        return Err(TechnicalObservationError::UnsupportedDisplayLumaContract {
            provided: contract_version,
            supported: DISPLAY_LUMA_CONTRACT_VERSION,
        });
    }
    if width == 0 || height == 0 {
        return Err(TechnicalObservationError::ZeroDimension);
    }
    validate_dimension("width", width)?;
    validate_dimension("height", height)?;
    if stride < width {
        return Err(TechnicalObservationError::StrideTooSmall { width, stride });
    }
    validate_provenance("preprocessing version", preprocessing_version)?;
    validate_provenance("input source hash", input_source_hash)?;

    let width = usize::try_from(width).map_err(|_| TechnicalObservationError::SizeOverflow)?;
    let height = usize::try_from(height).map_err(|_| TechnicalObservationError::SizeOverflow)?;
    let stride = usize::try_from(stride).map_err(|_| TechnicalObservationError::SizeOverflow)?;
    let active = width
        .checked_mul(height)
        .ok_or(TechnicalObservationError::SizeOverflow)?;
    if active > MAX_ACTIVE_LUMA_SAMPLES {
        return Err(TechnicalObservationError::TooManyActiveSamples {
            actual: active,
            maximum: MAX_ACTIVE_LUMA_SAMPLES,
        });
    }
    let expected = stride
        .checked_mul(height)
        .ok_or(TechnicalObservationError::SizeOverflow)?;
    if expected > MAX_DISPLAY_LUMA_BUFFER_SAMPLES {
        return Err(TechnicalObservationError::BufferTooLarge {
            actual: expected,
            maximum: MAX_DISPLAY_LUMA_BUFFER_SAMPLES,
        });
    }
    Ok(ValidDimensions {
        width,
        height,
        stride,
    })
}

fn validate_dimension(field: &'static str, value: u32) -> Result<(), TechnicalObservationError> {
    if value > MAX_DISPLAY_LUMA_DIMENSION {
        return Err(TechnicalObservationError::DimensionTooLarge {
            field,
            value,
            maximum: MAX_DISPLAY_LUMA_DIMENSION,
        });
    }
    Ok(())
}

fn validate_provenance(field: &'static str, value: &str) -> Result<(), TechnicalObservationError> {
    if value.trim().is_empty() {
        return Err(TechnicalObservationError::EmptyProvenance { field });
    }
    if value.len() > MAX_PROVENANCE_FIELD_BYTES {
        return Err(TechnicalObservationError::ProvenanceTooLong {
            field,
            maximum: MAX_PROVENANCE_FIELD_BYTES,
        });
    }
    Ok(())
}

fn collect_luma(
    plane: &DisplayLumaPlane<'_>,
    dimensions: ValidDimensions,
) -> Result<LumaAggregates, TechnicalObservationError> {
    let mut histogram = [0_u64; LUMA_HISTOGRAM_BIN_COUNT];
    let mut sorted_luma = Vec::with_capacity(dimensions.width * dimensions.height);
    let mut sum = 0.0;
    let mut near_black = 0;
    let mut near_white = 0;
    for y in 0..dimensions.height {
        let row = &plane.samples[y * dimensions.stride..][..dimensions.width];
        for (x, &luma) in row.iter().enumerate() {
            validate_luma(luma, x, y)?;
            histogram[histogram_bin(luma)] += 1;
            sum += f64::from(luma);
            near_black += u32::from(luma <= NEAR_BLACK_LUMA_THRESHOLD);
            near_white += u32::from(luma >= NEAR_WHITE_LUMA_THRESHOLD);
            sorted_luma.push(luma);
        }
    }
    sorted_luma.sort_unstable_by(f32::total_cmp);
    Ok(LumaAggregates {
        histogram,
        sorted_luma,
        sum,
        near_black,
        near_white,
    })
}

fn validate_luma(luma: f32, x: usize, y: usize) -> Result<(), TechnicalObservationError> {
    let x = u32::try_from(x).map_err(|_| TechnicalObservationError::SizeOverflow)?;
    let y = u32::try_from(y).map_err(|_| TechnicalObservationError::SizeOverflow)?;
    if !luma.is_finite() {
        return Err(TechnicalObservationError::NonFiniteLuma { x, y });
    }
    if !(0.0..=1.0).contains(&luma) {
        return Err(TechnicalObservationError::LumaOutOfRange { x, y, value: luma });
    }
    Ok(())
}

fn histogram_bin(luma: f32) -> usize {
    // `luma` was proven finite and in 0..=1 immediately before this call.
    #[allow(clippy::cast_possible_truncation, clippy::cast_sign_loss)]
    let scaled = (luma * 256.0).floor() as usize;
    scaled.min(LUMA_HISTOGRAM_BIN_COUNT - 1)
}

fn nearest_rank(sorted: &[f32], numerator: usize, denominator: usize) -> f64 {
    let index = (numerator * sorted.len() - 1) / denominator;
    f64::from(sorted[index])
}

fn sharpness_proxies(
    plane: &DisplayLumaPlane<'_>,
    dimensions: ValidDimensions,
) -> Result<(f64, f64), TechnicalObservationError> {
    let edge_energy = neighbor_edge_energy(plane, dimensions);
    if dimensions.width < 3 || dimensions.height < 3 {
        return Ok((0.0, edge_energy));
    }
    let mut count = 0_u32;
    let mut sum = 0.0;
    let mut square_sum = 0.0;
    for y in 1..dimensions.height - 1 {
        for x in 1..dimensions.width - 1 {
            let center = sample(plane, dimensions, x, y);
            let laplacian = 4.0 * center
                - sample(plane, dimensions, x - 1, y)
                - sample(plane, dimensions, x + 1, y)
                - sample(plane, dimensions, x, y - 1)
                - sample(plane, dimensions, x, y + 1);
            count += 1;
            sum += laplacian;
            square_sum += laplacian * laplacian;
        }
    }
    let denominator = f64::from(count);
    let mean = sum / denominator;
    let variance = (square_sum / denominator - mean * mean).max(0.0);
    if variance.is_finite() && edge_energy.is_finite() {
        Ok((variance, edge_energy))
    } else {
        Err(TechnicalObservationError::InvalidNonNegativeMetric(
            f64::NAN,
        ))
    }
}

fn neighbor_edge_energy(plane: &DisplayLumaPlane<'_>, dimensions: ValidDimensions) -> f64 {
    let mut sum = 0.0;
    let mut count = 0_u32;
    for y in 0..dimensions.height {
        for x in 0..dimensions.width {
            let center = sample(plane, dimensions, x, y);
            if x + 1 < dimensions.width {
                let difference = sample(plane, dimensions, x + 1, y) - center;
                sum += difference * difference;
                count += 1;
            }
            if y + 1 < dimensions.height {
                let difference = sample(plane, dimensions, x, y + 1) - center;
                sum += difference * difference;
                count += 1;
            }
        }
    }
    if count == 0 {
        0.0
    } else {
        sum / f64::from(count)
    }
}

fn sample(plane: &DisplayLumaPlane<'_>, dimensions: ValidDimensions, x: usize, y: usize) -> f64 {
    f64::from(plane.samples[y * dimensions.stride + x])
}

fn unit(value: f64) -> Result<UnitInterval, TechnicalObservationError> {
    UnitInterval::new(value).map_err(|_| TechnicalObservationError::InvalidComputedUnitMetric)
}

#[cfg(test)]
mod tests {
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
        unknown_implementation["algorithm"]["implementation_version"] =
            serde_json::json!("unknown-v2");
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
}
