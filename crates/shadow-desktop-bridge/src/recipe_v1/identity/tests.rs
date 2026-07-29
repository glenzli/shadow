use shadow_domain::{LayerInstanceId, MaskBrushPoint, MaskDefinition, UnitInterval};

use super::recipe_v1_local_mask_revision;

fn unit(value: f64) -> UnitInterval {
    UnitInterval::new(value).expect("test unit interval")
}

fn grade_node_id() -> LayerInstanceId {
    "018f0000-0000-7000-8000-000000000001"
        .parse()
        .expect("stable Grade Node id")
}

#[test]
fn legacy_local_mask_identity_goldens_remain_exact() {
    let masks = [
        MaskDefinition::linear_gradient(unit(0.1), unit(0.2), unit(0.8), unit(0.9), true)
            .expect("linear mask"),
        MaskDefinition::radial_gradient(
            unit(0.4),
            unit(0.6),
            unit(0.2),
            unit(0.3),
            unit(0.5),
            false,
        )
        .expect("radial mask"),
        MaskDefinition::brush(
            vec![MaskBrushPoint::new(unit(0.25), unit(0.75), true)],
            unit(0.04),
            unit(0.6),
            true,
        )
        .expect("brush mask"),
    ];
    let actual = masks.map(|mask| {
        recipe_v1_local_mask_revision(grade_node_id(), &mask)
            .expect("derive mask revision")
            .id()
            .to_string()
    });
    assert_eq!(
        actual,
        [
            "a4841ae5-991f-85d4-826e-00d51e0eefd2".to_owned(),
            "4bd744c6-3dfe-8d4c-b96e-66175b865137".to_owned(),
            "141a4c29-30dc-895c-8ca1-c9f1750cac6c".to_owned(),
        ]
    );
}

#[test]
fn condition_mask_identity_is_stable_distinct_and_uses_canonical_hue() {
    let luminance = MaskDefinition::luminance_range(unit(0.2), unit(0.8), unit(0.15), false)
        .expect("luminance range");
    let changed_luminance =
        MaskDefinition::luminance_range(unit(0.2), unit(0.7), unit(0.15), false)
            .expect("changed luminance range");
    let color = MaskDefinition::color_range(5.0, 35.0, unit(0.4), false).expect("color range");
    let wrapped_color =
        MaskDefinition::color_range(725.0, 35.0, unit(0.4), false).expect("wrapped color range");
    let changed_color =
        MaskDefinition::color_range(5.0, 36.0, unit(0.4), false).expect("changed color range");

    let identity = |mask: &MaskDefinition| {
        recipe_v1_local_mask_revision(grade_node_id(), mask)
            .expect("derive condition mask revision")
            .id()
    };
    assert_eq!(identity(&luminance), identity(&luminance));
    assert_ne!(identity(&luminance), identity(&changed_luminance));
    assert_eq!(identity(&color), identity(&wrapped_color));
    assert_ne!(identity(&color), identity(&changed_color));
    assert_ne!(identity(&luminance), identity(&color));
}
