use image::{ImageFormat, Rgb, RgbImage};
use shadow_ai::{FaceLandmarks, FacePoint};

use super::*;

#[test]
fn region_composition_unions_selected_ontology_classes() {
    let parsed = parsed_person(vec![1, 4, 5, 10, 11, 12, 13, 17], 4, 2);
    let regions =
        FaceRegionSet::from_bits(u32::from(FaceRegion::Eyes.bit() | FaceRegion::Hair.bit()))
            .unwrap();

    assert_eq!(
        compose_face_region_mask(&parsed, regions).unwrap(),
        vec![0, 255, 255, 0, 0, 0, 0, 255]
    );
    assert!(parsed.available_regions.contains(FaceRegion::Eyes));
    assert!(parsed.available_regions.contains(FaceRegion::Hair));
    assert!(!parsed.available_regions.contains(FaceRegion::Accessories));
}

#[test]
fn an_absent_combination_fails_instead_of_staging_an_empty_mask() {
    let parsed = parsed_person(vec![1, 1, 1, 1], 2, 2);
    let accessories = FaceRegionSet::from_bits(u32::from(FaceRegion::Accessories.bit())).unwrap();

    assert!(matches!(
        compose_face_region_mask(&parsed, accessories),
        Err(SubjectMaskPeopleError::SelectedRegionsUnavailable)
    ));
}

#[test]
fn detected_people_are_spatially_ordered_with_bounded_transient_thumbnails() {
    let mut image = RgbImage::new(200, 100);
    for (x, _y, pixel) in image.enumerate_pixels_mut() {
        *pixel = if x < 100 {
            Rgb([220, 80, 60])
        } else {
            Rgb([60, 100, 220])
        };
    }
    let mut jpeg = Vec::new();
    DynamicImage::ImageRgb8(image)
        .write_to(&mut Cursor::new(&mut jpeg), ImageFormat::Jpeg)
        .unwrap();
    let detections = [
        detected_face(120.0, 20.0, 50.0, 60.0, 0.98),
        detected_face(20.0, 20.0, 50.0, 60.0, 0.97),
    ];

    let people =
        prepare_person_candidates(&jpeg, RasterExtent::new(200, 100).unwrap(), &detections)
            .unwrap();

    assert_eq!(people.len(), 2);
    assert_eq!(people[0].bounding_box.x, 20.0);
    assert_eq!(people[1].bounding_box.x, 120.0);
    for person in people {
        assert!(!person.thumbnail_jpeg.is_empty());
        let thumbnail = image::load_from_memory(&person.thumbnail_jpeg).unwrap();
        assert_eq!(thumbnail.width(), PERSON_THUMBNAIL_EDGE);
        assert_eq!(thumbnail.height(), PERSON_THUMBNAIL_EDGE);
    }
}

fn parsed_person(labels: Vec<u8>, width: u32, height: u32) -> ParsedSubjectMaskPerson {
    ParsedSubjectMaskPerson::new(
        labels,
        RasterExtent::new(width, height).unwrap(),
        VisionProvenance {
            job_id: "job".into(),
            provider: "provider".into(),
            deployment: "deployment".into(),
            model_build: "build".into(),
            artifact_sha256: "artifact".into(),
            preprocessing_identity: "pre".into(),
            postprocessing_identity: "post".into(),
            tokenizer: None,
            runtime: "runtime".into(),
            requested_execution_provider: "cpu".into(),
            actual_execution_provider: "cpu".into(),
            execution_provider_fallback_reason: None,
            precision: "fp32".into(),
        },
    )
    .unwrap()
}

fn detected_face(x: f32, y: f32, width: f32, height: f32, confidence: f32) -> DetectedFace {
    let point = FacePoint { x, y };
    DetectedFace {
        bounding_box: FaceBoundingBox {
            x,
            y,
            width,
            height,
        },
        landmarks: FaceLandmarks {
            right_eye: point,
            left_eye: point,
            nose_tip: point,
            right_mouth_corner: point,
            left_mouth_corner: point,
        },
        confidence,
    }
}
