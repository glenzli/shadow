//! Conservative noise advice from one provider-neutral staged Bayer plane.
//!
//! This is deliberately separate from model execution. The estimator samples
//! matching CFA sites and uses the checkerboard residual of each 2x2 cell in
//! one colour plane. That residual cancels constant and linear scene content,
//! then a robust per-signal-bin median limits edge and texture contamination.

use std::{
    fs::{self, File},
    io::{Read, Seek, SeekFrom},
    path::{Path, PathBuf},
};

use thiserror::Error;

const STAGING_SCHEMA: &str = "shadow-raw-frame-staging-v1";
const MAX_MANIFEST_BYTES: u64 = 16 * 1024;
const MAX_SAMPLE_BYTES: u64 = 512 * 1024 * 1024;
const MAX_SAMPLED_ROWS_PER_SITE: usize = 96;
const MAX_SAMPLED_COLUMNS_PER_SITE: usize = 128;
const MIN_BIN_SAMPLES: usize = 96;
const GAUSSIAN_MEDIAN_ABSOLUTE_SCALE: f64 = 1.482_602_218_505_602;

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub(crate) enum RawFoundationNoiseLevel {
    Low,
    Moderate,
    High,
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub(crate) struct RawFoundationNoiseAssessment {
    pub(crate) level: RawFoundationNoiseLevel,
    pub(crate) score_percent: u8,
    pub(crate) confidence_percent: u8,
    pub(crate) diagnostic: String,
}

#[derive(Debug, Error)]
pub(crate) enum RawFoundationNoiseAssessmentError {
    #[error("read staged RAW manifest: {0}")]
    ReadManifest(#[source] std::io::Error),
    #[error("staged RAW manifest is invalid: {0}")]
    InvalidManifest(&'static str),
    #[error("read staged RAW samples: {0}")]
    ReadSamples(#[source] std::io::Error),
    #[error("staged RAW contains too few usable Bayer samples")]
    InsufficientSamples,
}

#[derive(Debug)]
struct StagedBayerManifest {
    width: usize,
    height: usize,
    black: [u16; 4],
    white: [u16; 4],
    sample_bytes: u64,
    sample_path: PathBuf,
}

#[derive(Debug, Copy, Clone)]
struct NoiseBlock {
    signal: f64,
    absolute_residual: f64,
}

#[derive(Debug, Copy, Clone)]
struct BinEstimate {
    relative_noise: f64,
    tail_ratio: f64,
    sample_count: usize,
}

pub(crate) fn assess_staged_bayer_noise(
    manifest_path: &Path,
) -> Result<RawFoundationNoiseAssessment, RawFoundationNoiseAssessmentError> {
    let manifest = parse_manifest(manifest_path)?;
    let metadata = fs::metadata(&manifest.sample_path)
        .map_err(RawFoundationNoiseAssessmentError::ReadSamples)?;
    if !metadata.is_file() || metadata.len() != manifest.sample_bytes {
        return Err(RawFoundationNoiseAssessmentError::InvalidManifest(
            "sample payload length does not match the manifest",
        ));
    }

    let mut samples = File::open(&manifest.sample_path)
        .map_err(RawFoundationNoiseAssessmentError::ReadSamples)?;
    let mut blocks =
        Vec::with_capacity(4 * MAX_SAMPLED_ROWS_PER_SITE * MAX_SAMPLED_COLUMNS_PER_SITE);
    for site in 0..4 {
        sample_site_blocks(&manifest, &mut samples, site, &mut blocks)?;
    }
    classify_blocks(&blocks)
}

fn parse_manifest(
    manifest_path: &Path,
) -> Result<StagedBayerManifest, RawFoundationNoiseAssessmentError> {
    let metadata =
        fs::metadata(manifest_path).map_err(RawFoundationNoiseAssessmentError::ReadManifest)?;
    if !metadata.is_file() || metadata.len() == 0 || metadata.len() > MAX_MANIFEST_BYTES {
        return Err(RawFoundationNoiseAssessmentError::InvalidManifest(
            "manifest size is outside the staging contract",
        ));
    }
    let text = fs::read_to_string(manifest_path)
        .map_err(RawFoundationNoiseAssessmentError::ReadManifest)?;
    let mut fields = text.split_whitespace();
    if fields.next() != Some(STAGING_SCHEMA) {
        return Err(RawFoundationNoiseAssessmentError::InvalidManifest(
            "unsupported schema",
        ));
    }

    let mut width = None;
    let mut height = None;
    let mut cfa = None;
    let mut black = None;
    let mut white = None;
    let mut sample_bytes = None;
    for field in fields {
        let Some((key, value)) = field.split_once('=') else {
            return Err(RawFoundationNoiseAssessmentError::InvalidManifest(
                "malformed field",
            ));
        };
        match key {
            "width" => width = value.parse::<usize>().ok(),
            "height" => height = value.parse::<usize>().ok(),
            "cfa" => cfa = Some(value.to_owned()),
            "black" => black = parse_level_array(value),
            "white" => white = parse_level_array(value),
            "sample_bytes" => sample_bytes = value.parse::<u64>().ok(),
            _ => {}
        }
    }
    let width = width.ok_or(RawFoundationNoiseAssessmentError::InvalidManifest(
        "missing width",
    ))?;
    let height = height.ok_or(RawFoundationNoiseAssessmentError::InvalidManifest(
        "missing height",
    ))?;
    let cfa = cfa.ok_or(RawFoundationNoiseAssessmentError::InvalidManifest(
        "missing CFA",
    ))?;
    let black = black.ok_or(RawFoundationNoiseAssessmentError::InvalidManifest(
        "missing black levels",
    ))?;
    let white = white.ok_or(RawFoundationNoiseAssessmentError::InvalidManifest(
        "missing white levels",
    ))?;
    let sample_bytes = sample_bytes.ok_or(RawFoundationNoiseAssessmentError::InvalidManifest(
        "missing sample byte count",
    ))?;
    if width < 8 || height < 8 || cfa.len() != 4 {
        return Err(RawFoundationNoiseAssessmentError::InvalidManifest(
            "Bayer extent or CFA is invalid",
        ));
    }
    let mut colours = cfa.bytes().collect::<Vec<_>>();
    colours.sort_unstable();
    if colours != [b'B', b'G', b'G', b'R'] {
        return Err(RawFoundationNoiseAssessmentError::InvalidManifest(
            "CFA is not a supported 2x2 Bayer layout",
        ));
    }
    if (0..4).any(|site| white[site] <= black[site]) {
        return Err(RawFoundationNoiseAssessmentError::InvalidManifest(
            "white level does not exceed black level",
        ));
    }
    let expected_bytes = (width as u64)
        .checked_mul(height as u64)
        .and_then(|value| value.checked_mul(2))
        .ok_or(RawFoundationNoiseAssessmentError::InvalidManifest(
            "sample extent overflows",
        ))?;
    if sample_bytes != expected_bytes || sample_bytes > MAX_SAMPLE_BYTES {
        return Err(RawFoundationNoiseAssessmentError::InvalidManifest(
            "sample byte count is invalid",
        ));
    }
    let mut sample_name = manifest_path.as_os_str().to_os_string();
    sample_name.push(".u16le");
    Ok(StagedBayerManifest {
        width,
        height,
        black,
        white,
        sample_bytes,
        sample_path: PathBuf::from(sample_name),
    })
}

fn parse_level_array(value: &str) -> Option<[u16; 4]> {
    let values = value
        .split(',')
        .map(str::parse::<u16>)
        .collect::<Result<Vec<_>, _>>()
        .ok()?;
    values.try_into().ok()
}

fn sample_site_blocks(
    manifest: &StagedBayerManifest,
    samples: &mut File,
    site: usize,
    output: &mut Vec<NoiseBlock>,
) -> Result<(), RawFoundationNoiseAssessmentError> {
    let x_parity = site % 2;
    let y_parity = site / 2;
    let plane_width = (manifest.width - x_parity).div_ceil(2);
    let plane_height = (manifest.height - y_parity).div_ceil(2);
    if plane_width < 2 || plane_height < 2 {
        return Ok(());
    }
    let rows = sampled_indices(plane_height - 1, MAX_SAMPLED_ROWS_PER_SITE);
    let columns = sampled_indices(plane_width - 1, MAX_SAMPLED_COLUMNS_PER_SITE);
    let mut upper = vec![0_u16; manifest.width];
    let mut lower = vec![0_u16; manifest.width];
    let black = f64::from(manifest.black[site]);
    let range = f64::from(manifest.white[site] - manifest.black[site]);
    for plane_y in rows {
        let raw_y = y_parity + plane_y * 2;
        read_sample_row(samples, manifest.width, raw_y, &mut upper)?;
        read_sample_row(samples, manifest.width, raw_y + 2, &mut lower)?;
        for &plane_x in &columns {
            let raw_x = x_parity + plane_x * 2;
            let values = [
                (f64::from(upper[raw_x]) - black) / range,
                (f64::from(upper[raw_x + 2]) - black) / range,
                (f64::from(lower[raw_x]) - black) / range,
                (f64::from(lower[raw_x + 2]) - black) / range,
            ];
            let signal = values.iter().sum::<f64>() * 0.25;
            if !(0.005..0.8).contains(&signal) {
                continue;
            }
            // For a locally planar scene a + d - b - c is exactly zero. The
            // division by two makes the residual standard deviation equal to
            // the per-sample noise standard deviation for independent noise.
            let residual = ((values[0] + values[3] - values[1] - values[2]) * 0.5).abs();
            if residual.is_finite() {
                output.push(NoiseBlock {
                    signal,
                    absolute_residual: residual,
                });
            }
        }
    }
    Ok(())
}

fn sampled_indices(count: usize, maximum: usize) -> Vec<usize> {
    if count <= maximum {
        return (0..count).collect();
    }
    (0..maximum)
        .map(|index| index * (count - 1) / (maximum - 1))
        .collect()
}

fn read_sample_row(
    samples: &mut File,
    width: usize,
    row: usize,
    output: &mut [u16],
) -> Result<(), RawFoundationNoiseAssessmentError> {
    let byte_offset = (row as u64)
        .checked_mul(width as u64)
        .and_then(|value| value.checked_mul(2))
        .ok_or(RawFoundationNoiseAssessmentError::InvalidManifest(
            "row offset overflows",
        ))?;
    samples
        .seek(SeekFrom::Start(byte_offset))
        .map_err(RawFoundationNoiseAssessmentError::ReadSamples)?;
    let mut encoded = vec![0_u8; width * 2];
    samples
        .read_exact(&mut encoded)
        .map_err(RawFoundationNoiseAssessmentError::ReadSamples)?;
    for (destination, pair) in output.iter_mut().zip(encoded.chunks_exact(2)) {
        *destination = u16::from_le_bytes([pair[0], pair[1]]);
    }
    Ok(())
}

fn classify_blocks(
    blocks: &[NoiseBlock],
) -> Result<RawFoundationNoiseAssessment, RawFoundationNoiseAssessmentError> {
    const SIGNAL_BINS: [(f64, f64, f64); 4] = [
        (0.005, 0.04, 0.65),
        (0.04, 0.12, 0.90),
        (0.12, 0.30, 1.00),
        (0.30, 0.80, 0.80),
    ];
    let total_blocks = blocks.len();
    let mut estimates = Vec::new();
    for (low, high, perceptual_weight) in SIGNAL_BINS {
        let selected = blocks
            .iter()
            .filter(|block| block.signal >= low && block.signal < high)
            .copied()
            .collect::<Vec<_>>();
        if selected.len() < MIN_BIN_SAMPLES {
            continue;
        }
        let mut signals = selected
            .iter()
            .map(|block| block.signal)
            .collect::<Vec<_>>();
        let mut residuals = selected
            .iter()
            .map(|block| block.absolute_residual)
            .collect::<Vec<_>>();
        let signal = quantile(&mut signals, 1, 2);
        let median_residual = quantile(&mut residuals, 1, 2);
        let tail_residual = quantile(&mut residuals, 9, 10);
        let sigma = median_residual * GAUSSIAN_MEDIAN_ABSOLUTE_SCALE;
        estimates.push(BinEstimate {
            relative_noise: (sigma / signal.max(0.005)) * perceptual_weight,
            tail_ratio: tail_residual / median_residual.max(1.0e-9),
            sample_count: selected.len(),
        });
    }
    let Some(dominant) = estimates
        .iter()
        .max_by(|left, right| left.relative_noise.total_cmp(&right.relative_noise))
        .copied()
    else {
        return Err(RawFoundationNoiseAssessmentError::InsufficientSamples);
    };

    let level = if dominant.relative_noise < 0.015 {
        RawFoundationNoiseLevel::Low
    } else if dominant.relative_noise < 0.032 {
        RawFoundationNoiseLevel::Moderate
    } else {
        RawFoundationNoiseLevel::High
    };
    let score_percent = noise_score(dominant.relative_noise);
    let estimated_samples = estimates
        .iter()
        .map(|estimate| estimate.sample_count)
        .sum::<usize>();
    let coverage = if total_blocks == 0 {
        0.0
    } else {
        count_as_f64(estimated_samples) / count_as_f64(total_blocks)
    };
    let mut confidence = match estimates.len() {
        0 | 1 => 54.0,
        2 => 70.0,
        _ => 84.0,
    };
    confidence += (coverage * 12.0).min(12.0);
    if dominant.sample_count < 256 {
        confidence -= 12.0;
    }
    // A very long residual tail is evidence that diagonal texture or repeated
    // pattern content is contaminating the otherwise robust median estimate.
    if dominant.tail_ratio > 8.0 {
        confidence -= 24.0;
    } else if dominant.tail_ratio > 5.0 {
        confidence -= 12.0;
    }
    let confidence_percent = rounded_bounded_u8(confidence.clamp(20.0, 96.0), 96);
    Ok(RawFoundationNoiseAssessment {
        level,
        score_percent,
        confidence_percent,
        diagnostic: format!(
            "staged Bayer residual v1 · {} bins · {} samples",
            estimates.len(),
            estimated_samples
        ),
    })
}

fn quantile(values: &mut [f64], numerator: usize, denominator: usize) -> f64 {
    values.sort_unstable_by(f64::total_cmp);
    debug_assert!(denominator > 0 && numerator <= denominator);
    let index = ((values.len() - 1) * numerator + denominator / 2) / denominator;
    values[index]
}

fn count_as_f64(value: usize) -> f64 {
    // The assessor samples at most 4 * 96 * 128 blocks, so this conversion is
    // exact and remains explicit if that bounded sampling contract changes.
    f64::from(u32::try_from(value).expect("noise sample count exceeds its bounded contract"))
}

fn rounded_bounded_u8(value: f64, maximum: u8) -> u8 {
    let bounded = value.clamp(0.0, f64::from(maximum));
    let mut rounded = 0_u8;
    while rounded < maximum && f64::from(rounded) + 0.5 <= bounded {
        rounded += 1;
    }
    rounded
}

fn noise_score(relative_noise: f64) -> u8 {
    let score = if relative_noise <= 0.008 {
        5.0 * relative_noise / 0.008
    } else if relative_noise <= 0.015 {
        5.0 + (relative_noise - 0.008) / 0.007 * 30.0
    } else if relative_noise <= 0.032 {
        35.0 + (relative_noise - 0.015) / 0.017 * 35.0
    } else {
        70.0 + ((relative_noise - 0.032) / 0.038).clamp(0.0, 1.0) * 30.0
    };
    rounded_bounded_u8(score, 100)
}

#[cfg(test)]
mod tests;
