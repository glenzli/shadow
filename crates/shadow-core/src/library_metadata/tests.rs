use std::{
    fs,
    sync::atomic::{AtomicU64, Ordering},
    time::{SystemTime, UNIX_EPOCH},
};

use shadow_domain::{EntityId, PhotoId};

use super::{GpsMatchSettings, GpsPhotoCapture, load_gpx_track, match_photos_to_gpx};

fn temporary_gpx(contents: &str) -> std::path::PathBuf {
    static NEXT_FIXTURE: AtomicU64 = AtomicU64::new(0);
    let nonce = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .expect("clock")
        .as_nanos();
    let sequence = NEXT_FIXTURE.fetch_add(1, Ordering::Relaxed);
    let path = std::env::temp_dir().join(format!(
        "shadow-gpx-{}-{nonce}-{sequence}.gpx",
        std::process::id()
    ));
    fs::write(&path, contents).expect("write GPX fixture");
    path
}

#[test]
fn parser_and_matcher_interpolate_timed_track_points() {
    let path = temporary_gpx(
        r#"<?xml version="1.0"?>
        <gpx version="1.1" creator="Shadow test">
          <trk><trkseg>
            <trkpt lat="31.0" lon="121.0"><ele>10</ele><time>2024-01-01T00:00:00Z</time></trkpt>
            <trkpt lat="32.0" lon="122.0"><ele>20</ele><time>2024-01-01T00:02:00Z</time></trkpt>
          </trkseg></trk>
        </gpx>"#,
    );
    let track = load_gpx_track(&path).expect("load GPX");
    let photo = GpsPhotoCapture {
        photo_id: PhotoId::new_v7(),
        captured_at_unix_seconds: 1_704_067_260,
    };
    let preview = match_photos_to_gpx(
        &track,
        &[photo],
        GpsMatchSettings {
            camera_clock_offset_seconds: 0,
            maximum_gap_seconds: 90,
        },
    )
    .expect("match GPX");
    fs::remove_file(path).expect("remove GPX fixture");

    assert_eq!(preview.proposals.len(), 1);
    assert_eq!(preview.unmatched_photo_count, 0);
    assert_eq!(preview.proposals[0].coordinates.latitude_e7, 315_000_000);
    assert_eq!(preview.proposals[0].coordinates.longitude_e7, 1_215_000_000);
    assert_eq!(preview.proposals[0].nearest_track_delta_seconds, 60);
}

#[test]
fn matcher_applies_camera_clock_offset() {
    let path = temporary_gpx(
        r#"<gpx><trk><trkseg>
          <trkpt lat="10" lon="20"><time>2024-01-01T01:00:00+01:00</time></trkpt>
        </trkseg></trk></gpx>"#,
    );
    let track = load_gpx_track(&path).expect("load GPX");
    let photo = GpsPhotoCapture {
        photo_id: PhotoId::new_v7(),
        captured_at_unix_seconds: 1_704_063_600,
    };
    let preview = match_photos_to_gpx(
        &track,
        &[photo],
        GpsMatchSettings {
            camera_clock_offset_seconds: 3_600,
            maximum_gap_seconds: 30,
        },
    )
    .expect("match GPX");
    fs::remove_file(path).expect("remove GPX fixture");

    assert_eq!(preview.proposals.len(), 1);
    assert_eq!(preview.unmatched_photo_count, 0);
    assert_eq!(preview.proposals[0].coordinates.latitude_e7, 100_000_000);
}
