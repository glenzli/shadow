use super::{parse_iso_6709, parse_probe_response, supports};

#[test]
fn accepts_apple_iso_6709_location_and_utc_creation_time() {
    let metadata = parse_probe_response(
        br#"{
            "format": {
                "tags": {
                    "creation_time": "2026-08-13T04:30:00.000000Z",
                    "com.apple.quicktime.location.ISO": "+39.901434+116.421122+43.2/"
                }
            }
        }"#,
    )
    .expect("parse probe response")
    .expect("metadata anchor");
    assert_eq!(metadata.captured_at_unix_seconds, 1_786_595_400);
    assert!((metadata.latitude_degrees - 39.901_434).abs() < f64::EPSILON);
    assert!((metadata.longitude_degrees - 116.421_122).abs() < f64::EPSILON);
}

#[test]
fn accepts_quicktime_creation_offset_without_a_colon() {
    let metadata = parse_probe_response(
        br#"{
            "format": {
                "tags": {
                    "com.apple.quicktime.creationdate": "2026-08-13T12:30:00+0800",
                    "location": "+39.901434+116.421122/"
                }
            }
        }"#,
    )
    .expect("parse quicktime response")
    .expect("metadata anchor");
    assert_eq!(metadata.captured_at_unix_seconds, 1_786_595_400);
}

#[test]
fn rejects_incomplete_or_out_of_range_container_metadata() {
    assert_eq!(parse_iso_6709("+91.0+116.0/"), None);
    assert_eq!(parse_iso_6709("39.9,116.4"), None);
    assert!(
        parse_probe_response(br#"{"format":{"tags":{"creation_time":"2026-08-13T04:30:00Z"}}}"#)
            .expect("parse incomplete response")
            .is_none()
    );
}

#[test]
fn restricts_video_metadata_probe_to_supported_containers() {
    assert!(supports(std::path::Path::new("clip.MOV")));
    assert!(supports(std::path::Path::new("clip.mp4")));
    assert!(!supports(std::path::Path::new("photo.jpg")));
}
