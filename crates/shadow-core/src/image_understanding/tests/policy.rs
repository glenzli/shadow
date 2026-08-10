use super::super::*;

#[test]
fn default_policy_limits_background_work_to_liked_photos() {
    let policy = ImageUnderstandingScanPolicy::default();

    assert_eq!(policy.scope(), ImageUnderstandingScanScope::Liked);
    assert!(policy.includes(true, 0));
    assert!(!policy.includes(false, 5));
    assert_eq!(policy.catalog_filters().len(), 1);
    assert_eq!(policy.catalog_filters()[0].liked, Some(true));
    assert_eq!(policy.minimum_rating(), None);
}

#[test]
fn all_and_rating_scopes_compile_to_bounded_catalog_filters() {
    let all = ImageUnderstandingScanPolicy::new(
        ImageUnderstandingScanScope::All,
        None,
        DEFAULT_IMAGE_UNDERSTANDING_BATCH_SIZE,
    )
    .expect("all policy");
    assert_eq!(
        all.catalog_filters(),
        vec![shadow_catalog::LibraryPhotoFilter::default()]
    );
    assert!(all.includes(false, 0));

    let rated =
        ImageUnderstandingScanPolicy::new(ImageUnderstandingScanScope::MinimumRating, Some(4), 2)
            .expect("rating policy");
    assert_eq!(rated.catalog_filters()[0].minimum_rating, Some(4));
    assert!(!rated.includes(true, 3));
    assert!(rated.includes(false, 4));
}

#[test]
fn liked_or_rating_uses_two_filters_and_one_stable_revision() {
    let policy = ImageUnderstandingScanPolicy::new(
        ImageUnderstandingScanScope::LikedOrMinimumRating,
        Some(5),
        4,
    )
    .expect("combined policy");
    let filters = policy.catalog_filters();

    assert_eq!(filters.len(), 2);
    assert_eq!(filters[0].liked, Some(true));
    assert_eq!(filters[1].minimum_rating, Some(5));
    assert!(policy.includes(true, 0));
    assert!(policy.includes(false, 5));
    assert!(!policy.includes(false, 4));
    assert_eq!(
        policy.revision(),
        "shadow.image-understanding-scan:v1:liked_or_minimum_rating:rating-5:batch-4"
    );
}

#[test]
fn invalid_thresholds_and_batches_fail_closed() {
    assert_eq!(
        ImageUnderstandingScanPolicy::new(ImageUnderstandingScanScope::Liked, Some(5), 4),
        Err(ImageUnderstandingPolicyError::UnexpectedRatingThreshold)
    );
    assert_eq!(
        ImageUnderstandingScanPolicy::new(ImageUnderstandingScanScope::MinimumRating, Some(0), 4,),
        Err(ImageUnderstandingPolicyError::InvalidRatingThreshold)
    );
    assert_eq!(
        ImageUnderstandingScanPolicy::new(ImageUnderstandingScanScope::MinimumRating, Some(6), 4,),
        Err(ImageUnderstandingPolicyError::InvalidRatingThreshold)
    );
    assert_eq!(
        ImageUnderstandingScanPolicy::new(ImageUnderstandingScanScope::All, None, 0),
        Err(ImageUnderstandingPolicyError::InvalidBatchSize(0))
    );
    assert_eq!(
        ImageUnderstandingScanPolicy::new(
            ImageUnderstandingScanScope::All,
            None,
            MAX_IMAGE_UNDERSTANDING_BATCH_SIZE + 1,
        ),
        Err(ImageUnderstandingPolicyError::InvalidBatchSize(
            MAX_IMAGE_UNDERSTANDING_BATCH_SIZE + 1
        ))
    );
}
