use std::path::Path;

use shadow_domain::RepresentationKind;

use super::grouping_for_path;

#[test]
fn raw_and_jpeg_share_a_source_relative_group_while_derived_rasters_do_not() {
    let root = Path::new("/photos");
    let raw = grouping_for_path(
        root,
        Path::new("/photos/Trip/IMG_0001.NEF"),
        RepresentationKind::OriginalRaw,
    )
    .expect("RAW grouping")
    .expect("RAW participates");
    let jpeg = grouping_for_path(
        root,
        Path::new("/photos/trip/img_0001.JPG"),
        RepresentationKind::OriginalRaster,
    )
    .expect("JPEG grouping")
    .expect("JPEG participates");
    assert_eq!(raw, jpeg);

    assert!(
        grouping_for_path(
            root,
            Path::new("/photos/Trip/IMG_0001.tiff"),
            RepresentationKind::OriginalRaster,
        )
        .expect("TIFF grouping")
        .is_none()
    );
}
