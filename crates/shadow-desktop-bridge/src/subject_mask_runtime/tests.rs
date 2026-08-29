use super::*;

#[test]
fn input_validation_keeps_provider_payload_bounded() {
    assert!(matches!(
        validate_input_jpeg(&[]),
        Err(SubjectMaskRuntimeError::InputSize(0))
    ));
    validate_input_jpeg(b"bounded JPEG bytes").expect("bounded input");
}

#[test]
fn face_region_composition_maps_only_the_selected_ontology_classes() {
    let extent = RasterExtent::new(4, 2).unwrap();
    let labels = [1, 4, 5, 10, 11, 12, 13, 17];

    assert_eq!(
        compose_face_region_mask(&labels, extent, FaceRegion::Eyes).unwrap(),
        vec![0, 255, 255, 0, 0, 0, 0, 0]
    );
    assert_eq!(
        compose_face_region_mask(&labels, extent, FaceRegion::LipsAndMouth).unwrap(),
        vec![0, 0, 0, 0, 255, 255, 255, 0]
    );
    assert_eq!(
        compose_face_region_mask(&labels, extent, FaceRegion::Hair).unwrap(),
        vec![0, 0, 0, 0, 0, 0, 0, 255]
    );
}

#[test]
fn absent_face_region_fails_instead_of_staging_an_empty_mask() {
    let extent = RasterExtent::new(2, 2).unwrap();
    assert!(matches!(
        compose_face_region_mask(&[1, 1, 1, 1], extent, FaceRegion::Accessories),
        Err(SubjectMaskRuntimeError::FaceRegionUnavailable)
    ));
}
