use super::measured_plan;

#[test]
fn comparable_midtones_get_a_bounded_subject_lift() {
    let plan = measured_plan(&[110, 115, 125, 130], &[50; 4], &[255, 255, 0, 0]);
    assert_eq!(plan.reason, 1);
    assert!(!plan.background);
    assert!(plan.exposure > 0.0 && plan.exposure <= 0.2);
}

#[test]
fn bright_subject_is_protected_by_restraining_background() {
    let plan = measured_plan(
        &[230, 235, 180, 180],
        &[50, 50, 120, 120],
        &[255, 255, 0, 0],
    );
    assert_eq!(plan.reason, 2);
    assert!(plan.background && plan.exposure < 0.0 && plan.exposure >= -0.15);
}

#[test]
fn silhouettes_and_already_separated_subjects_stay_unchanged() {
    for light in [[15, 15, 180, 180], [180, 180, 40, 40]] {
        let plan = measured_plan(&light, &[50; 4], &[255, 255, 0, 0]);
        assert_eq!(plan.reason, 3);
        assert_eq!(plan.exposure, 0.0);
    }
}

#[test]
fn empty_full_and_uncertain_masks_do_not_authorize_an_adjustment() {
    for mask in [[0; 4], [255; 4], [128; 4]] {
        assert_eq!(measured_plan(&[110; 4], &[50; 4], &mask).reason, 4);
    }
    assert_eq!(measured_plan(&[], &[], &[]).reason, 4);
}

#[test]
fn small_but_coherent_subjects_are_not_rejected_as_empty() {
    let mut mask = vec![0; 1_000];
    mask[..10].fill(255);
    assert_eq!(measured_plan(&[110; 1_000], &[50; 1_000], &mask).reason, 1);
    mask[..10].fill(0);
    mask[0] = 255;
    assert_eq!(measured_plan(&[110; 1_000], &[50; 1_000], &mask).reason, 4);
}
