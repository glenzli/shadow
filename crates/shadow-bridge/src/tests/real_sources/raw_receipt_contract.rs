//! RAW development and pipeline receipts across decoder, preview, and detail handles.

use std::path::PathBuf;

use crate::{
    RawDevelopmentPlan, RawDevelopmentReceipt, RawPipelineReceipt,
    decoder::open_libraw,
    raw_development::{raw_development_receipt, raw_pipeline_receipt},
};

#[test]
#[ignore = "requires SHADOW_TEST_DNG to point at a local RAW fixture"]
#[allow(clippy::too_many_lines)] // One ignored end-to-end bridge scenario spans all handles.
fn real_dng_raw_development_receipts_cross_all_prepared_handles() {
    let path = PathBuf::from(std::env::var_os("SHADOW_TEST_DNG").expect("SHADOW_TEST_DNG"));
    let handle = open_libraw(&path).expect("open local DNG");
    let decoder = handle.as_ref().expect("non-null decoder handle");

    let before_preparation = raw_development_receipt(
        decoder
            .raw_development_receipt()
            .expect("read empty RAW development receipt"),
    )
    .expect("bridge empty RAW development receipt");
    assert_eq!(before_preparation, RawDevelopmentReceipt::default());
    let pipeline_before_preparation = raw_pipeline_receipt(
        decoder
            .raw_pipeline_receipt()
            .expect("read empty RAW pipeline receipt"),
    )
    .expect("bridge empty RAW pipeline receipt");
    assert_eq!(pipeline_before_preparation, RawPipelineReceipt::default());

    let preview_handle = decoder
        .prepare_edit_preview(1_024)
        .expect("prepare local DNG warm preview");
    let preview = preview_handle.as_ref().expect("non-null preview handle");
    let preview_receipt = raw_development_receipt(
        preview
            .raw_development_receipt()
            .expect("read preview RAW development receipt"),
    )
    .expect("bridge preview RAW development receipt");
    assert!(preview_receipt.recorded());
    assert!(preview_receipt.uses_current_schema());
    assert_eq!(preview_receipt.provider_id, "libraw");
    let preview_pipeline_receipt = raw_pipeline_receipt(
        preview
            .raw_pipeline_receipt()
            .expect("read preview RAW pipeline receipt"),
    )
    .expect("bridge preview RAW pipeline receipt");
    assert!(preview_pipeline_receipt.recorded());
    assert_eq!(
        preview_pipeline_receipt.requested_plan,
        RawDevelopmentPlan::preview()
    );
    assert_eq!(
        raw_pipeline_receipt(
            decoder
                .raw_pipeline_receipt()
                .expect("read decoder preview RAW pipeline receipt"),
        )
        .expect("bridge decoder preview RAW pipeline receipt"),
        preview_pipeline_receipt
    );
    assert_eq!(
        raw_development_receipt(
            decoder
                .raw_development_receipt()
                .expect("read decoder preview RAW development receipt"),
        )
        .expect("bridge decoder preview RAW development receipt"),
        preview_receipt,
        "DecodeHandle reports the last source render it prepared"
    );

    let detail_handle = decoder
        .prepare_edit_detail()
        .expect("prepare local DNG full detail");
    let detail = detail_handle.as_ref().expect("non-null detail handle");
    let detail_receipt = raw_development_receipt(
        detail
            .raw_development_receipt()
            .expect("read detail RAW development receipt"),
    )
    .expect("bridge detail RAW development receipt");
    assert!(detail_receipt.recorded());
    assert!(detail_receipt.uses_current_schema());
    assert_eq!(detail_receipt.provider_id, "libraw");
    assert!(!detail_receipt.half_size);
    let detail_pipeline_receipt = raw_pipeline_receipt(
        detail
            .raw_pipeline_receipt()
            .expect("read detail RAW pipeline receipt"),
    )
    .expect("bridge detail RAW pipeline receipt");
    assert!(detail_pipeline_receipt.recorded());
    assert_eq!(
        detail_pipeline_receipt.requested_plan,
        RawDevelopmentPlan::detail()
    );
    assert_eq!(
        raw_pipeline_receipt(
            decoder
                .raw_pipeline_receipt()
                .expect("read decoder detail RAW pipeline receipt"),
        )
        .expect("bridge decoder detail RAW pipeline receipt"),
        detail_pipeline_receipt
    );
    assert_eq!(
        raw_development_receipt(
            decoder
                .raw_development_receipt()
                .expect("read decoder detail RAW development receipt"),
        )
        .expect("bridge decoder detail RAW development receipt"),
        detail_receipt,
        "DecodeHandle updates its read-only receipt when a new source render is prepared"
    );
}
