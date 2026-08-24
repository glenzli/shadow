//! Lossless, source-stage CFA export as a standards-readable DNG.
//!
//! This owner deliberately bypasses the Recipe renderer. It streams the exact
//! active little-endian Bayer plane from isolated `RawFrame` staging into an
//! uncompressed classic-TIFF DNG, then publishes without overwriting an
//! existing destination. Standard DNG tags carry the portable projection;
//! `DNGPrivateData` retains Shadow's exact four-site descriptor.

use std::{
    fs::File,
    io::{Read as _, Seek as _, SeekFrom, Write as _},
    path::{Path, PathBuf},
};

use anyhow::{Context, Result, anyhow, bail};
use serde_json::json;
use sha2::{Digest as _, Sha256};
use tempfile::{Builder as TemporaryFileBuilder, NamedTempFile};

use crate::{
    digest_hex::encode_hex,
    isolated_proxy::{IsolatedRawFrameDescriptor, IsolatedRawFrameStaging},
};

const PRODUCT_DNG_SCHEMA: &str = "shadow.raw-dng-export.v1";
const WRITER_VERSION: &str = "20260825.1";
const PRIVATE_MAGIC: &[u8] = b"Shadow\0shadow.raw-dng-export.v1\0";
const COPY_BUFFER_BYTES: usize = 1024 * 1024;

const TYPE_BYTE: u16 = 1;
const TYPE_ASCII: u16 = 2;
const TYPE_SHORT: u16 = 3;
const TYPE_LONG: u16 = 4;
const TYPE_RATIONAL: u16 = 5;
const TYPE_SRATIONAL: u16 = 10;

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct RawDngEncodingReceipt {
    pub(crate) width: u32,
    pub(crate) height: u32,
    pub(crate) byte_len: u64,
    pub(crate) content_digest: [u8; 32],
    pub(crate) receipt_json: String,
}

#[derive(Debug)]
pub(crate) enum RawDngPublishOutcome {
    Published(RawDngEncodingReceipt),
    OutputConflict,
}

#[derive(Debug)]
pub(crate) struct PendingRawDng {
    temporary: NamedTempFile,
    destination: PathBuf,
    receipt: RawDngEncodingReceipt,
}

impl PendingRawDng {
    pub(crate) fn publish(self) -> Result<RawDngPublishOutcome> {
        match self.temporary.persist_noclobber(&self.destination) {
            Ok(_) => Ok(RawDngPublishOutcome::Published(self.receipt)),
            Err(error) if error.error.kind() == std::io::ErrorKind::AlreadyExists => {
                Ok(RawDngPublishOutcome::OutputConflict)
            }
            Err(error) => Err(error.error).with_context(|| {
                format!(
                    "publish RAW DNG atomically without overwrite {}",
                    self.destination.display()
                )
            }),
        }
    }
}

pub(crate) fn encode_to_temporary(
    staging: &IsolatedRawFrameStaging,
    destination: &Path,
) -> Result<PendingRawDng> {
    let mut samples = staging
        .try_clone_sample()
        .context("clone staged RAW CFA sample handle for DNG export")?;
    encode_parts(staging.descriptor(), &mut samples, destination)
}

fn encode_parts(
    descriptor: &IsolatedRawFrameDescriptor,
    samples: &mut File,
    destination: &Path,
) -> Result<PendingRawDng> {
    let parent = destination
        .parent()
        .filter(|parent| !parent.as_os_str().is_empty())
        .ok_or_else(|| anyhow!("RAW DNG destination requires a parent directory"))?;
    if !destination.is_absolute() {
        bail!("RAW DNG destination must be absolute");
    }
    let mut temporary = TemporaryFileBuilder::new()
        .prefix(".shadow-raw-dng-")
        .tempfile_in(parent)
        .with_context(|| {
            format!(
                "create RAW DNG temporary output beside {}",
                destination.display()
            )
        })?;

    let projection = dng_projection(descriptor)?;
    let provisional = ifd_values(descriptor, &projection, 0)?;
    let (_, strip_offset) = layout_ifd(&provisional)?;
    let values = ifd_values(descriptor, &projection, strip_offset)?;
    let (ifd, confirmed_strip_offset) = layout_ifd(&values)?;
    if confirmed_strip_offset != strip_offset {
        bail!("RAW DNG strip layout did not converge");
    }
    let header = [b'I', b'I', 42, 0, 8, 0, 0, 0];
    let mut dng_hasher = Sha256::new();
    write_hashed(temporary.as_file_mut(), &mut dng_hasher, &header)?;
    write_hashed(temporary.as_file_mut(), &mut dng_hasher, &ifd)?;

    samples
        .seek(SeekFrom::Start(0))
        .context("rewind staged RAW CFA samples")?;
    let mut sample_hasher = Sha256::new();
    let mut copied = 0_u64;
    let mut buffer = vec![0_u8; COPY_BUFFER_BYTES];
    loop {
        let count = samples
            .read(&mut buffer)
            .context("read staged RAW CFA samples")?;
        if count == 0 {
            break;
        }
        temporary
            .as_file_mut()
            .write_all(&buffer[..count])
            .context("write RAW DNG CFA strip")?;
        sample_hasher.update(&buffer[..count]);
        dng_hasher.update(&buffer[..count]);
        copied = copied
            .checked_add(u64::try_from(count).unwrap_or(u64::MAX))
            .ok_or_else(|| anyhow!("RAW DNG CFA byte count overflow"))?;
    }
    if copied != descriptor.sample_bytes {
        bail!("staged RAW CFA sample length changed during DNG encoding");
    }
    let sample_digest = sample_hasher.finalize();
    if encode_hex(sample_digest.as_slice()) != descriptor.decoded_samples_sha256 {
        bail!("staged RAW CFA sample digest changed during DNG encoding");
    }
    temporary
        .as_file_mut()
        .sync_all()
        .context("sync RAW DNG temporary output")?;
    let byte_len = temporary
        .as_file()
        .metadata()
        .context("inspect RAW DNG temporary output")?
        .len();
    let content_digest: [u8; 32] = dng_hasher.finalize().into();
    let receipt_json = serde_json::to_string(&json!({
        "schema": "shadow-raw-dng-output-receipt-20260825.1",
        "format": "dng",
        "bit_depth": 16,
        "width": descriptor.width,
        "height": descriptor.height,
        "cfa": descriptor.cfa,
        "source_sample_sha256": descriptor.decoded_samples_sha256,
        "dng_sha256": encode_hex(&content_digest),
        "standard_white_level": projection.standard_white_level,
        "standard_white_projection": projection.standard_white_projection,
        "standard_neutral_projection": projection.standard_neutral_projection,
        "calibration_projection": projection.calibration_projection,
        "decoder_provider_id": descriptor.decoder_provider_id,
        "decoder_provider_version": descriptor.decoder_provider_version,
        "recipe_applied": false,
        "pending_opcodes_applied": false,
    }))
    .context("serialize RAW DNG output receipt")?;

    Ok(PendingRawDng {
        temporary,
        destination: destination.to_path_buf(),
        receipt: RawDngEncodingReceipt {
            width: descriptor.width,
            height: descriptor.height,
            byte_len,
            content_digest,
            receipt_json,
        },
    })
}

fn write_hashed(file: &mut File, hasher: &mut Sha256, bytes: &[u8]) -> Result<()> {
    file.write_all(bytes).context("write RAW DNG structure")?;
    hasher.update(bytes);
    Ok(())
}

#[derive(Debug, Clone, PartialEq)]
struct DngProjection {
    standard_white_level: u32,
    standard_white_projection: &'static str,
    neutral: [f64; 3],
    standard_neutral_projection: &'static str,
    matrix: [f64; 9],
    illuminant: u16,
    calibration_projection: &'static str,
}

fn dng_projection(descriptor: &IsolatedRawFrameDescriptor) -> Result<DngProjection> {
    let standard_white_level = u32::from(
        *descriptor
            .white_levels
            .iter()
            .min()
            .ok_or_else(|| anyhow!("RAW DNG has no sensor white levels"))?,
    );
    let standard_white_projection = if descriptor
        .white_levels
        .iter()
        .all(|level| *level == descriptor.white_levels[0])
    {
        "exact"
    } else {
        "conservative-minimum"
    };
    let mut sums = [0.0_f64; 3];
    let mut counts = [0_u32; 3];
    let mut green_values = Vec::with_capacity(2);
    for (index, colour) in descriptor.cfa.bytes().enumerate() {
        let plane = match colour {
            b'R' => 0,
            b'G' => 1,
            b'B' => 2,
            _ => bail!("RAW DNG CFA contains an unsupported colour"),
        };
        sums[plane] += descriptor.as_shot_neutral[index];
        counts[plane] += 1;
        if colour == b'G' {
            green_values.push(descriptor.as_shot_neutral[index]);
        }
    }
    if counts.contains(&0) {
        bail!("RAW DNG CFA does not contain R, G, and B");
    }
    let neutral = std::array::from_fn(|index| sums[index] / f64::from(counts[index]));
    let standard_neutral_projection =
        if green_values.len() < 2 || green_values.windows(2).all(|pair| pair[0] == pair[1]) {
            "exact"
        } else {
            "green-site-average"
        };
    let (matrix, illuminant, calibration_projection) = camera_matrix(descriptor)?;
    Ok(DngProjection {
        standard_white_level,
        standard_white_projection,
        neutral,
        standard_neutral_projection,
        matrix,
        illuminant,
        calibration_projection,
    })
}

fn camera_matrix(descriptor: &IsolatedRawFrameDescriptor) -> Result<([f64; 9], u16, &'static str)> {
    if let Some(matrix) = descriptor.xyz_to_camera_d65 {
        return Ok((matrix, 21, "xyz-to-camera-d65-exact"));
    }
    if let Some(matrix) = descriptor.camera_to_xyz_d50 {
        return Ok((inverse_3x3(matrix)?, 23, "inverse-camera-to-xyz-d50"));
    }
    if let Some(camera_to_linear_srgb) = descriptor.camera_to_linear_srgb_d65 {
        let linear_srgb_to_xyz_d65 = [
            0.412_456_4,
            0.357_576_1,
            0.180_437_5,
            0.212_672_9,
            0.715_152_2,
            0.072_175,
            0.019_333_9,
            0.119_192,
            0.950_304_1,
        ];
        return Ok((
            inverse_3x3(multiply_3x3(linear_srgb_to_xyz_d65, camera_to_linear_srgb))?,
            21,
            "derived-from-camera-to-linear-srgb-d65",
        ));
    }
    Ok((
        [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0],
        21,
        "identity-fallback",
    ))
}

fn inverse_3x3(matrix: [f64; 9]) -> Result<[f64; 9]> {
    let [a, b, c, d, e, f, g, h, i] = matrix;
    let determinant =
        a * e.mul_add(i, -(f * h)) - b * d.mul_add(i, -(f * g)) + c * d.mul_add(h, -(e * g));
    if !determinant.is_finite() || determinant.abs() < 1.0e-12 {
        bail!("RAW DNG camera calibration matrix is singular");
    }
    Ok([
        e.mul_add(i, -(f * h)) / determinant,
        c.mul_add(h, -(b * i)) / determinant,
        b.mul_add(f, -(c * e)) / determinant,
        f.mul_add(g, -(d * i)) / determinant,
        a.mul_add(i, -(c * g)) / determinant,
        c.mul_add(d, -(a * f)) / determinant,
        d.mul_add(h, -(e * g)) / determinant,
        b.mul_add(g, -(a * h)) / determinant,
        a.mul_add(e, -(b * d)) / determinant,
    ])
}

fn multiply_3x3(left: [f64; 9], right: [f64; 9]) -> [f64; 9] {
    std::array::from_fn(|index| {
        let row = index / 3;
        let column = index % 3;
        (0..3)
            .map(|inner| left[row * 3 + inner] * right[inner * 3 + column])
            .sum()
    })
}

fn dng_orientation(libraw_orientation: i32) -> Result<u16> {
    match libraw_orientation {
        0 => Ok(1),
        3 => Ok(3),
        5 => Ok(8),
        6 => Ok(6),
        _ => bail!("RAW DNG orientation is unsupported"),
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct IfdValue {
    tag: u16,
    field_type: u16,
    count: u32,
    data: Vec<u8>,
}

fn ifd_values(
    descriptor: &IsolatedRawFrameDescriptor,
    projection: &DngProjection,
    strip_offset: u32,
) -> Result<Vec<IfdValue>> {
    let cfa_pattern = descriptor
        .cfa
        .bytes()
        .map(|colour| match colour {
            b'R' => Ok(0),
            b'G' => Ok(1),
            b'B' => Ok(2),
            _ => bail!("RAW DNG CFA contains an unsupported colour"),
        })
        .collect::<Result<Vec<_>>>()?;
    let private_payload = private_payload(descriptor, projection)?;
    let sample_bytes = u32::try_from(descriptor.sample_bytes)
        .context("RAW DNG CFA strip exceeds classic TIFF bounds")?;
    let software = format!("Shadow RAW DNG {WRITER_VERSION}");
    let mut values = vec![
        unsigned_value(254, TYPE_LONG, &[0])?,
        unsigned_value(256, TYPE_LONG, &[descriptor.width])?,
        unsigned_value(257, TYPE_LONG, &[descriptor.height])?,
        unsigned_value(258, TYPE_SHORT, &[16])?,
        unsigned_value(259, TYPE_SHORT, &[1])?,
        unsigned_value(262, TYPE_SHORT, &[32_803])?,
        unsigned_value(273, TYPE_LONG, &[strip_offset])?,
        unsigned_value(
            274,
            TYPE_SHORT,
            &[u32::from(dng_orientation(descriptor.orientation)?)],
        )?,
        unsigned_value(277, TYPE_SHORT, &[1])?,
        unsigned_value(278, TYPE_LONG, &[descriptor.height])?,
        unsigned_value(279, TYPE_LONG, &[sample_bytes])?,
        unsigned_value(284, TYPE_SHORT, &[1])?,
        ascii_value(305, &software)?,
        unsigned_value(33_421, TYPE_SHORT, &[2, 2])?,
        byte_value(33_422, &cfa_pattern)?,
        byte_value(50_706, &[1, 4, 0, 0])?,
        byte_value(50_707, &[1, 1, 0, 0])?,
        ascii_value(50_708, "Shadow normalized RawFrame")?,
        byte_value(50_710, &[0, 1, 2])?,
        unsigned_value(50_711, TYPE_SHORT, &[1])?,
        unsigned_value(50_713, TYPE_SHORT, &[2, 2])?,
        rational_value(50_714, &descriptor.black_levels.map(f64::from), false)?,
        unsigned_value(50_717, TYPE_LONG, &[projection.standard_white_level])?,
        rational_value(50_718, &[1.0, 1.0], false)?,
        unsigned_value(50_719, TYPE_LONG, &[0, 0])?,
        unsigned_value(50_720, TYPE_LONG, &[descriptor.width, descriptor.height])?,
        rational_value(50_721, &projection.matrix, true)?,
        rational_value(50_728, &projection.neutral, false)?,
        byte_value(50_740, &private_payload)?,
        unsigned_value(50_778, TYPE_SHORT, &[u32::from(projection.illuminant)])?,
        unsigned_value(
            50_829,
            TYPE_LONG,
            &[0, 0, descriptor.height, descriptor.width],
        )?,
    ];
    values.sort_unstable_by_key(|value| value.tag);
    Ok(values)
}

fn private_payload(
    descriptor: &IsolatedRawFrameDescriptor,
    projection: &DngProjection,
) -> Result<Vec<u8>> {
    let document = json!({
        "schema": PRODUCT_DNG_SCHEMA,
        "writer_version": WRITER_VERSION,
        "normalized_mosaic": {
            "schema": "shadow-raw-frame-staging-20260822.1",
            "descriptor_contract": "active-camera-colour-response-20260822.1",
            "width": descriptor.width,
            "height": descriptor.height,
            "cfa": descriptor.cfa,
            "black": descriptor.black_levels,
            "white": descriptor.white_levels,
            "linear_response": descriptor.linear_response_limits,
            "has_linear_response": descriptor.has_linear_response_limits,
            "orientation": descriptor.orientation,
            "bits_per_sample": descriptor.bits_per_sample,
            "as_shot_neutral": descriptor.as_shot_neutral,
            "camera_to_xyz_d50": descriptor.camera_to_xyz_d50,
            "xyz_to_camera_d65": descriptor.xyz_to_camera_d65,
            "camera_to_linear_srgb_d65": descriptor.camera_to_linear_srgb_d65,
            "pending_dng_opcode_bytes": descriptor.pending_dng_opcode_bytes,
            "provider_id": descriptor.decoder_provider_id,
            "provider_version": descriptor.decoder_provider_version,
            "sample_bytes": descriptor.sample_bytes,
            "sample_sha256": descriptor.decoded_samples_sha256,
        },
        "standard_projection": {
            "white_level": projection.standard_white_level,
            "white": projection.standard_white_projection,
            "neutral": projection.standard_neutral_projection,
            "calibration": projection.calibration_projection,
        },
        "pending_opcodes_applied": false,
        "geometry": "active-area-only-unoriented",
        "recipe_applied": false,
    });
    let mut payload = PRIVATE_MAGIC.to_vec();
    payload.extend(
        serde_json::to_vec(&document).context("serialize exact RAW DNG private descriptor")?,
    );
    Ok(payload)
}

fn unsigned_value(tag: u16, field_type: u16, values: &[u32]) -> Result<IfdValue> {
    let mut data = Vec::new();
    match field_type {
        TYPE_BYTE => {
            for value in values {
                data.push(u8::try_from(*value).context("encode TIFF byte value")?);
            }
        }
        TYPE_SHORT => {
            for value in values {
                data.extend(
                    u16::try_from(*value)
                        .context("encode TIFF short value")?
                        .to_le_bytes(),
                );
            }
        }
        TYPE_LONG => {
            for value in values {
                data.extend(value.to_le_bytes());
            }
        }
        _ => bail!("unsupported TIFF unsigned field type"),
    }
    Ok(IfdValue {
        tag,
        field_type,
        count: u32::try_from(values.len()).context("encode TIFF value count")?,
        data,
    })
}

fn byte_value(tag: u16, bytes: &[u8]) -> Result<IfdValue> {
    Ok(IfdValue {
        tag,
        field_type: TYPE_BYTE,
        count: u32::try_from(bytes.len()).context("encode TIFF byte count")?,
        data: bytes.to_vec(),
    })
}

fn ascii_value(tag: u16, value: &str) -> Result<IfdValue> {
    if !value.is_ascii() || value.as_bytes().contains(&0) {
        bail!("RAW DNG ASCII tag contains unsupported text");
    }
    let mut data = value.as_bytes().to_vec();
    data.push(0);
    Ok(IfdValue {
        tag,
        field_type: TYPE_ASCII,
        count: u32::try_from(data.len()).context("encode TIFF ASCII count")?,
        data,
    })
}

fn rational_value(tag: u16, values: &[f64], signed: bool) -> Result<IfdValue> {
    let mut data = Vec::with_capacity(values.len().saturating_mul(8));
    for value in values {
        let (numerator, denominator) = bounded_fraction(*value, signed)?;
        if signed {
            data.extend(
                i32::try_from(numerator)
                    .context("encode TIFF signed rational numerator")?
                    .to_le_bytes(),
            );
            data.extend(
                i32::try_from(denominator)
                    .context("encode TIFF signed rational denominator")?
                    .to_le_bytes(),
            );
        } else {
            data.extend(
                u32::try_from(numerator)
                    .context("encode TIFF rational numerator")?
                    .to_le_bytes(),
            );
            data.extend(
                u32::try_from(denominator)
                    .context("encode TIFF rational denominator")?
                    .to_le_bytes(),
            );
        }
    }
    Ok(IfdValue {
        tag,
        field_type: if signed {
            TYPE_SRATIONAL
        } else {
            TYPE_RATIONAL
        },
        count: u32::try_from(values.len()).context("encode TIFF rational count")?,
        data,
    })
}

fn bounded_fraction(value: f64, signed: bool) -> Result<(i64, i64)> {
    if !value.is_finite() || (!signed && value < 0.0) {
        bail!("RAW DNG rational value is unsupported");
    }
    const DENOMINATOR: i64 = 1_000_000;
    let scaled = value * DENOMINATOR as f64;
    let minimum = if signed { f64::from(i32::MIN) } else { 0.0 };
    let maximum = if signed {
        f64::from(i32::MAX)
    } else {
        f64::from(u32::MAX)
    };
    if scaled < minimum || scaled > maximum {
        bail!("RAW DNG rational value exceeds classic TIFF bounds");
    }
    let numerator = scaled.round() as i64;
    let divisor = greatest_common_divisor(numerator.unsigned_abs(), DENOMINATOR as u64);
    Ok((
        numerator / i64::try_from(divisor).unwrap_or(1),
        DENOMINATOR / i64::try_from(divisor).unwrap_or(1),
    ))
}

fn greatest_common_divisor(mut left: u64, mut right: u64) -> u64 {
    while right != 0 {
        let remainder = left % right;
        left = right;
        right = remainder;
    }
    left.max(1)
}

fn layout_ifd(values: &[IfdValue]) -> Result<(Vec<u8>, u32)> {
    let entry_count = u16::try_from(values.len()).context("RAW DNG has too many TIFF tags")?;
    let ifd_size = 2_u32
        .checked_add(u32::from(entry_count).saturating_mul(12))
        .and_then(|size| size.checked_add(4))
        .ok_or_else(|| anyhow!("RAW DNG TIFF directory size overflow"))?;
    let extra_offset = 8_u32
        .checked_add(ifd_size)
        .ok_or_else(|| anyhow!("RAW DNG TIFF offset overflow"))?;
    let mut entries = Vec::with_capacity(usize::from(entry_count).saturating_mul(12));
    let mut extra = Vec::new();
    for value in values {
        let type_size = match value.field_type {
            TYPE_BYTE | TYPE_ASCII => 1_u32,
            TYPE_SHORT => 2,
            TYPE_LONG => 4,
            TYPE_RATIONAL | TYPE_SRATIONAL => 8,
            _ => bail!("RAW DNG TIFF field type is unsupported"),
        };
        let expected = type_size
            .checked_mul(value.count)
            .ok_or_else(|| anyhow!("RAW DNG TIFF payload size overflow"))?;
        if usize::try_from(expected).ok() != Some(value.data.len()) {
            bail!("RAW DNG TIFF tag payload length is inconsistent");
        }
        entries.extend(value.tag.to_le_bytes());
        entries.extend(value.field_type.to_le_bytes());
        entries.extend(value.count.to_le_bytes());
        if value.data.len() <= 4 {
            entries.extend(&value.data);
            entries.resize(entries.len() + (4 - value.data.len()), 0);
        } else {
            align_four(extra_offset, &mut extra)?;
            let relative =
                u32::try_from(extra.len()).context("RAW DNG TIFF extra data overflow")?;
            entries.extend(
                extra_offset
                    .checked_add(relative)
                    .ok_or_else(|| anyhow!("RAW DNG TIFF data offset overflow"))?
                    .to_le_bytes(),
            );
            extra.extend(&value.data);
        }
    }
    align_four(extra_offset, &mut extra)?;
    let mut ifd = Vec::with_capacity(
        usize::try_from(ifd_size)
            .unwrap_or_default()
            .saturating_add(extra.len()),
    );
    ifd.extend(entry_count.to_le_bytes());
    ifd.extend(entries);
    ifd.extend(0_u32.to_le_bytes());
    ifd.extend(extra);
    let strip_offset = 8_u32
        .checked_add(u32::try_from(ifd.len()).context("RAW DNG TIFF directory exceeds bounds")?)
        .ok_or_else(|| anyhow!("RAW DNG strip offset overflow"))?;
    Ok((ifd, strip_offset))
}

fn align_four(base_offset: u32, bytes: &mut Vec<u8>) -> Result<()> {
    while (usize::try_from(base_offset).unwrap_or_default() + bytes.len()) % 4 != 0 {
        bytes.push(0);
    }
    Ok(())
}

#[cfg(test)]
mod tests;
