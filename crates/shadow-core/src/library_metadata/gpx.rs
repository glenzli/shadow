//! Bounded GPX track parsing and deterministic capture-time matching.

use std::{
    cmp::Ordering,
    fs::File,
    io::{BufReader, Read},
    path::{Path, PathBuf},
};

use quick_xml::{Reader, events::Event};
use shadow_catalog::LibraryCoordinates;
use shadow_domain::PhotoId;
use time::{OffsetDateTime, format_description::well_known::Rfc3339};

const MAX_GPX_BYTES: u64 = 256 * 1024 * 1024;
const MAX_TRACK_POINTS: usize = 2_000_000;
const MAX_TEXT_BYTES: usize = 256;

#[derive(Debug, thiserror::Error)]
pub enum GpxImportError {
    #[error("cannot read GPX file {path}: {source}")]
    Io {
        path: PathBuf,
        #[source]
        source: std::io::Error,
    },
    #[error("GPX file exceeds the {MAX_GPX_BYTES}-byte safety limit")]
    FileTooLarge,
    #[error("GPX XML is invalid: {0}")]
    Xml(#[from] quick_xml::Error),
    #[error("GPX text encoding is invalid: {0}")]
    Encoding(#[from] quick_xml::encoding::EncodingError),
    #[error("GPX contains more than {MAX_TRACK_POINTS} track points")]
    TooManyTrackPoints,
    #[error("GPX does not contain a track point with valid coordinates and RFC 3339 time")]
    NoTimedTrackPoints,
    #[error("maximum timestamp gap must be between 0 and 86400 seconds")]
    InvalidMaximumGap,
    #[error("camera clock offset is too large")]
    InvalidClockOffset,
}

#[derive(Debug, Copy, Clone, PartialEq)]
pub struct GpxTrackPoint {
    pub unix_seconds: i64,
    pub latitude_degrees: f64,
    pub longitude_degrees: f64,
    pub elevation_meters: Option<f64>,
}

#[derive(Debug, Clone, PartialEq)]
pub struct GpxTrack {
    pub source_path: PathBuf,
    pub source_digest: [u8; 32],
    pub points: Vec<GpxTrackPoint>,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub struct GpsPhotoCapture {
    pub photo_id: PhotoId,
    pub captured_at_unix_seconds: i64,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub struct GpsMatchSettings {
    /// Added to each camera timestamp before comparing it with UTC GPX time.
    pub camera_clock_offset_seconds: i64,
    pub maximum_gap_seconds: u32,
}

impl Default for GpsMatchSettings {
    fn default() -> Self {
        Self {
            camera_clock_offset_seconds: 0,
            maximum_gap_seconds: 300,
        }
    }
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct GpsMatchProposal {
    pub photo_id: PhotoId,
    pub captured_at_unix_seconds: i64,
    pub matched_at_unix_seconds: i64,
    pub nearest_track_delta_seconds: u32,
    pub coordinates: LibraryCoordinates,
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct GpsMatchPreview {
    pub source_path: PathBuf,
    pub source_digest: [u8; 32],
    pub settings: GpsMatchSettings,
    pub requested_photo_count: usize,
    pub unmatched_photo_count: usize,
    pub proposals: Vec<GpsMatchProposal>,
}

#[derive(Debug, Default)]
struct PendingTrackPoint {
    latitude_degrees: Option<f64>,
    longitude_degrees: Option<f64>,
    elevation_text: String,
    time_text: String,
    active_text: ActiveText,
}

#[derive(Debug, Default, Copy, Clone, Eq, PartialEq)]
enum ActiveText {
    #[default]
    None,
    Elevation,
    Time,
}

pub fn load_gpx_track(path: &Path) -> Result<GpxTrack, GpxImportError> {
    let metadata = std::fs::metadata(path).map_err(|source| io_error(path, source))?;
    if metadata.len() > MAX_GPX_BYTES {
        return Err(GpxImportError::FileTooLarge);
    }

    let digest = digest_file(path)?;
    let file = File::open(path).map_err(|source| io_error(path, source))?;
    let mut reader = Reader::from_reader(BufReader::new(file));
    reader.config_mut().trim_text(true);
    let mut buffer = Vec::new();
    let mut pending = None::<PendingTrackPoint>;
    let mut points = Vec::new();

    loop {
        match reader.read_event_into(&mut buffer)? {
            Event::Start(element) => {
                let name = element.local_name();
                match name.as_ref() {
                    b"trkpt" => {
                        pending = Some(PendingTrackPoint {
                            latitude_degrees: attribute_f64(&reader, &element, b"lat"),
                            longitude_degrees: attribute_f64(&reader, &element, b"lon"),
                            ..PendingTrackPoint::default()
                        });
                    }
                    b"ele" if pending.is_some() => {
                        pending.as_mut().expect("checked pending").active_text =
                            ActiveText::Elevation;
                    }
                    b"time" if pending.is_some() => {
                        pending.as_mut().expect("checked pending").active_text = ActiveText::Time;
                    }
                    _ => {}
                }
            }
            Event::Text(text) => {
                if let Some(point) = pending.as_mut() {
                    let decoded = text.xml_content()?;
                    match point.active_text {
                        ActiveText::Elevation => {
                            append_bounded(&mut point.elevation_text, &decoded);
                        }
                        ActiveText::Time => append_bounded(&mut point.time_text, &decoded),
                        ActiveText::None => {}
                    }
                }
            }
            Event::CData(text) => {
                if let Some(point) = pending.as_mut() {
                    let decoded = text.xml_content()?;
                    match point.active_text {
                        ActiveText::Elevation => {
                            append_bounded(&mut point.elevation_text, &decoded);
                        }
                        ActiveText::Time => append_bounded(&mut point.time_text, &decoded),
                        ActiveText::None => {}
                    }
                }
            }
            Event::End(element) => {
                let name = element.local_name();
                match name.as_ref() {
                    b"ele" | b"time" if pending.is_some() => {
                        pending.as_mut().expect("checked pending").active_text = ActiveText::None;
                    }
                    b"trkpt" => {
                        if let Some(point) = pending.take().and_then(finish_point) {
                            if points.len() >= MAX_TRACK_POINTS {
                                return Err(GpxImportError::TooManyTrackPoints);
                            }
                            points.push(point);
                        }
                    }
                    _ => {}
                }
            }
            Event::Eof => break,
            _ => {}
        }
        buffer.clear();
    }

    if points.is_empty() {
        return Err(GpxImportError::NoTimedTrackPoints);
    }
    points.sort_by_key(|point| point.unix_seconds);
    points.dedup_by_key(|point| point.unix_seconds);
    Ok(GpxTrack {
        source_path: path.to_path_buf(),
        source_digest: digest,
        points,
    })
}

pub fn match_photos_to_gpx(
    track: &GpxTrack,
    photos: &[GpsPhotoCapture],
    settings: GpsMatchSettings,
) -> Result<GpsMatchPreview, GpxImportError> {
    if settings.maximum_gap_seconds > 86_400 {
        return Err(GpxImportError::InvalidMaximumGap);
    }
    let maximum_offset = 14_i64 * 86_400;
    if !(-maximum_offset..=maximum_offset).contains(&settings.camera_clock_offset_seconds) {
        return Err(GpxImportError::InvalidClockOffset);
    }

    let mut proposals = Vec::with_capacity(photos.len());
    for photo in photos {
        let Some(target) = photo
            .captured_at_unix_seconds
            .checked_add(settings.camera_clock_offset_seconds)
        else {
            continue;
        };
        if let Some((coordinates, nearest_delta)) = coordinates_at(
            &track.points,
            target,
            i64::from(settings.maximum_gap_seconds),
        ) {
            proposals.push(GpsMatchProposal {
                photo_id: photo.photo_id,
                captured_at_unix_seconds: photo.captured_at_unix_seconds,
                matched_at_unix_seconds: target,
                nearest_track_delta_seconds: u32::try_from(nearest_delta)
                    .expect("bounded by maximum u32 gap"),
                coordinates,
            });
        }
    }
    Ok(GpsMatchPreview {
        source_path: track.source_path.clone(),
        source_digest: track.source_digest,
        settings,
        requested_photo_count: photos.len(),
        unmatched_photo_count: photos.len().saturating_sub(proposals.len()),
        proposals,
    })
}

fn coordinates_at(
    points: &[GpxTrackPoint],
    target: i64,
    maximum_gap: i64,
) -> Option<(LibraryCoordinates, i64)> {
    let index = points.partition_point(|point| point.unix_seconds < target);
    let before = index.checked_sub(1).and_then(|value| points.get(value));
    let after = points.get(index);

    if let Some(after) = after
        && after.unix_seconds == target
    {
        return Some((
            coordinates(after.latitude_degrees, after.longitude_degrees),
            0,
        ));
    }
    match (before, after) {
        (Some(before), Some(after)) => {
            let before_delta = target.checked_sub(before.unix_seconds)?;
            let after_delta = after.unix_seconds.checked_sub(target)?;
            if before_delta <= maximum_gap && after_delta <= maximum_gap {
                let span = after.unix_seconds.checked_sub(before.unix_seconds)?;
                if span == 0 {
                    return Some((
                        coordinates(before.latitude_degrees, before.longitude_degrees),
                        before_delta.min(after_delta),
                    ));
                }
                #[allow(clippy::cast_precision_loss)]
                let fraction = before_delta as f64 / span as f64;
                let latitude =
                    interpolate(before.latitude_degrees, after.latitude_degrees, fraction);
                let longitude = interpolate_longitude(
                    before.longitude_degrees,
                    after.longitude_degrees,
                    fraction,
                );
                return Some((
                    coordinates(latitude, longitude),
                    before_delta.min(after_delta),
                ));
            }
            nearest_endpoint(before, after, before_delta, after_delta, maximum_gap)
        }
        (Some(before), None) => {
            let delta = target.checked_sub(before.unix_seconds)?;
            (delta <= maximum_gap).then(|| {
                (
                    coordinates(before.latitude_degrees, before.longitude_degrees),
                    delta,
                )
            })
        }
        (None, Some(after)) => {
            let delta = after.unix_seconds.checked_sub(target)?;
            (delta <= maximum_gap).then(|| {
                (
                    coordinates(after.latitude_degrees, after.longitude_degrees),
                    delta,
                )
            })
        }
        (None, None) => None,
    }
}

fn nearest_endpoint(
    before: &GpxTrackPoint,
    after: &GpxTrackPoint,
    before_delta: i64,
    after_delta: i64,
    maximum_gap: i64,
) -> Option<(LibraryCoordinates, i64)> {
    let (point, delta) = match before_delta.cmp(&after_delta) {
        Ordering::Less | Ordering::Equal => (before, before_delta),
        Ordering::Greater => (after, after_delta),
    };
    (delta <= maximum_gap).then(|| {
        (
            coordinates(point.latitude_degrees, point.longitude_degrees),
            delta,
        )
    })
}

fn interpolate(start: f64, end: f64, fraction: f64) -> f64 {
    start + (end - start) * fraction
}

fn interpolate_longitude(start: f64, end: f64, fraction: f64) -> f64 {
    let mut delta = end - start;
    if delta > 180.0 {
        delta -= 360.0;
    } else if delta < -180.0 {
        delta += 360.0;
    }
    let value = start + delta * fraction;
    if value > 180.0 {
        value - 360.0
    } else if value < -180.0 {
        value + 360.0
    } else {
        value
    }
}

fn coordinates(latitude_degrees: f64, longitude_degrees: f64) -> LibraryCoordinates {
    LibraryCoordinates {
        latitude_e7: degrees_e7(latitude_degrees),
        longitude_e7: degrees_e7(longitude_degrees),
        place_name: String::new(),
    }
}

#[allow(clippy::cast_possible_truncation)]
fn degrees_e7(value: f64) -> i32 {
    (value * 10_000_000.0).round() as i32
}

fn finish_point(point: PendingTrackPoint) -> Option<GpxTrackPoint> {
    let latitude = point.latitude_degrees?;
    let longitude = point.longitude_degrees?;
    if !latitude.is_finite()
        || !longitude.is_finite()
        || !(-90.0..=90.0).contains(&latitude)
        || !(-180.0..=180.0).contains(&longitude)
    {
        return None;
    }
    let captured_at = OffsetDateTime::parse(point.time_text.trim(), &Rfc3339).ok()?;
    Some(GpxTrackPoint {
        unix_seconds: captured_at.unix_timestamp(),
        latitude_degrees: latitude,
        longitude_degrees: longitude,
        elevation_meters: point
            .elevation_text
            .trim()
            .parse::<f64>()
            .ok()
            .filter(|value| value.is_finite()),
    })
}

fn attribute_f64(
    reader: &Reader<BufReader<File>>,
    element: &quick_xml::events::BytesStart<'_>,
    expected: &[u8],
) -> Option<f64> {
    element
        .attributes()
        .with_checks(false)
        .filter_map(Result::ok)
        .find(|attribute| attribute.key.local_name().as_ref() == expected)
        .and_then(|attribute| {
            attribute
                .decode_and_unescape_value(reader.decoder())
                .ok()?
                .parse::<f64>()
                .ok()
        })
}

fn append_bounded(destination: &mut String, value: &str) {
    let remaining = MAX_TEXT_BYTES.saturating_sub(destination.len());
    destination.extend(value.chars().take(remaining));
}

fn digest_file(path: &Path) -> Result<[u8; 32], GpxImportError> {
    let mut file = File::open(path).map_err(|source| io_error(path, source))?;
    let mut hasher = blake3::Hasher::new();
    let mut buffer = [0_u8; 64 * 1024];
    loop {
        let read = file
            .read(&mut buffer)
            .map_err(|source| io_error(path, source))?;
        if read == 0 {
            break;
        }
        hasher.update(&buffer[..read]);
    }
    Ok(*hasher.finalize().as_bytes())
}

fn io_error(path: &Path, source: std::io::Error) -> GpxImportError {
    GpxImportError::Io {
        path: path.to_path_buf(),
        source,
    }
}
