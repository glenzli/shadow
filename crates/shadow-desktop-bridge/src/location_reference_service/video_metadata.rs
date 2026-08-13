//! Read-only video-container metadata for location-reference folders.
//!
//! The reference workflow needs only capture time and GPS. It therefore asks
//! `ffprobe` for container tags and never opens a video stream for decoding.

use std::{collections::BTreeMap, io::ErrorKind, path::Path, process::Command};

use anyhow::{Context, Result};
use serde::Deserialize;
use time::{OffsetDateTime, format_description::well_known::Rfc3339};

#[derive(Debug, Clone, Copy, PartialEq)]
pub(super) struct VideoReferenceMetadata {
    pub(super) captured_at_unix_seconds: i64,
    pub(super) latitude_degrees: f64,
    pub(super) longitude_degrees: f64,
}

#[derive(Debug, Deserialize)]
struct ProbeResponse {
    #[serde(default)]
    format: ProbeFormat,
}

#[derive(Debug, Default, Deserialize)]
struct ProbeFormat {
    #[serde(default)]
    tags: BTreeMap<String, String>,
}

#[must_use]
pub(super) fn supports(path: &Path) -> bool {
    path.extension()
        .and_then(|extension| extension.to_str())
        .is_some_and(|extension| {
            matches!(
                extension.to_ascii_lowercase().as_str(),
                "mov" | "mp4" | "m4v" | "3gp" | "3g2"
            )
        })
}

/// Reads tagged capture metadata only. Missing `ffprobe`, unreadable videos,
/// and files without both a timestamp and GPS return no anchor to retain the
/// reference-folder scan's best-effort semantics.
pub(super) fn inspect(path: &Path) -> Result<Option<VideoReferenceMetadata>> {
    if !supports(path) {
        return Ok(None);
    }
    let output = match Command::new(ffprobe_program())
        .args(["-v", "error", "-print_format", "json", "-show_format"])
        .arg(path)
        .output()
    {
        Ok(output) => output,
        Err(error) if error.kind() == ErrorKind::NotFound => return Ok(None),
        Err(error) => {
            return Err(error)
                .with_context(|| format!("run ffprobe for reference video {}", path.display()));
        }
    };
    if !output.status.success() {
        return Ok(None);
    }
    parse_probe_response(&output.stdout)
}

fn ffprobe_program() -> &'static str {
    [
        "/opt/homebrew/bin/ffprobe",
        "/usr/local/bin/ffprobe",
        "ffprobe",
    ]
    .into_iter()
    .find(|program| Path::new(program).is_file())
    .unwrap_or("ffprobe")
}

fn parse_probe_response(bytes: &[u8]) -> Result<Option<VideoReferenceMetadata>> {
    let response: ProbeResponse =
        serde_json::from_slice(bytes).context("parse ffprobe metadata")?;
    let tags = normalized_tags(response.format.tags);
    let Some(captured_at_unix_seconds) = capture_time(&tags) else {
        return Ok(None);
    };
    let Some((latitude_degrees, longitude_degrees)) = tags
        .get("com.apple.quicktime.location.iso")
        .or_else(|| tags.get("location"))
        .or_else(|| tags.get("location-eng"))
        .or_else(|| tags.get("gpscoordinates"))
        .and_then(|value| parse_iso_6709(value))
    else {
        return Ok(None);
    };
    Ok(Some(VideoReferenceMetadata {
        captured_at_unix_seconds,
        latitude_degrees,
        longitude_degrees,
    }))
}

fn normalized_tags(tags: BTreeMap<String, String>) -> BTreeMap<String, String> {
    tags.into_iter()
        .map(|(key, value)| (key.to_ascii_lowercase(), value))
        .collect()
}

fn capture_time(tags: &BTreeMap<String, String>) -> Option<i64> {
    ["creation_time", "com.apple.quicktime.creationdate", "date"]
        .into_iter()
        .filter_map(|key| tags.get(key))
        .find_map(|value| parse_capture_time(value))
        .map(OffsetDateTime::unix_timestamp)
}

fn parse_capture_time(value: &str) -> Option<OffsetDateTime> {
    let value = value.trim();
    OffsetDateTime::parse(value, &Rfc3339).ok().or_else(|| {
        let bytes = value.as_bytes();
        let zone_start = bytes
            .iter()
            .enumerate()
            .skip(10)
            .find_map(|(index, byte)| matches!(byte, b'+' | b'-').then_some(index))?;
        let zone = &value[zone_start..];
        if zone.len() != 5 || !zone[1..].bytes().all(|byte| byte.is_ascii_digit()) {
            return None;
        }
        let normalized = format!("{}{}:{}", &value[..zone_start], &zone[..3], &zone[3..]);
        OffsetDateTime::parse(&normalized, &Rfc3339).ok()
    })
}

fn parse_iso_6709(value: &str) -> Option<(f64, f64)> {
    let value = value.trim().trim_end_matches('/');
    let bytes = value.as_bytes();
    if !matches!(bytes.first(), Some(b'+' | b'-')) {
        return None;
    }
    let longitude_start = (1..bytes.len()).find(|&index| matches!(bytes[index], b'+' | b'-'))?;
    let altitude_start =
        ((longitude_start + 1)..bytes.len()).find(|&index| matches!(bytes[index], b'+' | b'-'));
    let latitude = value[..longitude_start].parse::<f64>().ok()?;
    let longitude = value[longitude_start..altitude_start.unwrap_or(bytes.len())]
        .parse::<f64>()
        .ok()?;
    if latitude.is_finite()
        && longitude.is_finite()
        && (-90.0..=90.0).contains(&latitude)
        && (-180.0..=180.0).contains(&longitude)
    {
        Some((latitude, longitude))
    } else {
        None
    }
}

#[cfg(test)]
mod tests;
