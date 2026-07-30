use shadow_domain::ImageDimensions;

use super::{
    RAW_FOUNDATION_IMPLEMENTATION_REVISION, RAW_FOUNDATION_MODEL_IDENTITY,
    RawFoundationArtifactIdentity, VerifiedRawFoundation,
};

const DIGEST_A: &str = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
const DIGEST_B: &str = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
const DIGEST_C: &str = "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";

fn identity() -> RawFoundationArtifactIdentity {
    RawFoundationArtifactIdentity::from_verified_digests(DIGEST_A, DIGEST_B, DIGEST_C)
        .expect("fixture digests are canonical")
}

#[test]
fn valid_transfer_seals_fixed_model_and_owned_pixels() {
    let transfer = VerifiedRawFoundation::from_verified_interleaved_camera_rgb(
        ImageDimensions {
            width: 2,
            height: 2,
        },
        1,
        0,
        identity(),
        vec![0.25; 12],
    )
    .expect("valid transfer");
    assert_eq!(
        transfer.dimensions(),
        ImageDimensions {
            width: 2,
            height: 2
        }
    );
    assert_eq!(transfer.crop_top(), 1);
    assert_eq!(transfer.crop_left(), 0);
    assert_eq!(transfer.ffi.model_identity, RAW_FOUNDATION_MODEL_IDENTITY);
    assert_eq!(
        transfer.ffi.implementation_revision,
        RAW_FOUNDATION_IMPLEMENTATION_REVISION
    );
    assert_eq!(transfer.ffi.samples, vec![0.25; 12]);
    assert_eq!(
        transfer.artifact_identity().artifact_file_sha256(),
        DIGEST_B
    );
}

#[test]
fn digest_crop_shape_and_finite_values_fail_closed() {
    assert!(
        RawFoundationArtifactIdentity::from_verified_digests(
            DIGEST_A.to_ascii_uppercase(),
            DIGEST_B,
            DIGEST_C,
        )
        .is_err()
    );
    assert!(
        VerifiedRawFoundation::from_verified_interleaved_camera_rgb(
            ImageDimensions {
                width: 2,
                height: 2,
            },
            2,
            0,
            identity(),
            vec![0.25; 12],
        )
        .is_err()
    );
    assert!(
        VerifiedRawFoundation::from_verified_interleaved_camera_rgb(
            ImageDimensions {
                width: 2,
                height: 2,
            },
            0,
            0,
            identity(),
            vec![0.25; 11],
        )
        .is_err()
    );
    let mut non_finite = vec![0.25; 12];
    non_finite[4] = f32::NAN;
    assert!(
        VerifiedRawFoundation::from_verified_interleaved_camera_rgb(
            ImageDimensions {
                width: 2,
                height: 2,
            },
            0,
            0,
            identity(),
            non_finite,
        )
        .is_err()
    );
}

#[test]
fn arithmetic_and_transfer_limits_fail_before_sample_access() {
    assert!(
        VerifiedRawFoundation::from_verified_interleaved_camera_rgb(
            ImageDimensions {
                width: u32::MAX,
                height: u32::MAX,
            },
            0,
            0,
            identity(),
            Vec::new(),
        )
        .is_err()
    );
}
