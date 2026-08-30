//! Recipe v1 allocation and round-trip identity contracts.

use shadow_bridge::{OklabLightnessToneCurve, ToneCurvePoint};
use shadow_domain::{EntityId, NodeId, PhotoCanvasNode, PhotoFoundationNode};
use uuid::Uuid;

use crate::{
    ffi,
    recipe_v1::{
        GradeNodeDraft, GradeStackDraft, decode_grade_stack_draft_from_recipe_v1_snapshot,
        decode_grade_stack_draft_recipe_v1, encode_grade_stack_draft_recipe_v1,
        grade_stack_recipe_v1_snapshot, new_basic_grade_node,
        recipe_v1_oklab_lightness_tone_curve_render_op_id, recipe_v1_perceptual_color_render_op_id,
        recipe_v1_selective_tone_render_op_id, recipe_v1_sharpen_render_op_id,
        single_grade_node_recipe_v1_identity, single_grade_node_recipe_v1_render_ops,
    },
};

#[test]
#[allow(clippy::too_many_lines)]
fn new_basic_grade_node_allocates_complete_stable_identity_and_round_trips() {
    let created = new_basic_grade_node("Portrait foundation").expect("new Basic Grade Node");
    for value in [
        &created.grade_node_id,
        &created.exposure_render_op_id,
        &created.contrast_render_op_id,
        &created.white_balance_render_op_id,
        &created.saturation_render_op_id,
    ] {
        let id = Uuid::parse_str(value).expect("UUID identity");
        assert_eq!(id.get_version_num(), 7);
    }
    let decoded = decode_grade_stack_draft_recipe_v1(&ffi::FfiEditSettings {
        foundation: ffi::FfiPhotoFoundationSettings {
            enabled: true,
            raw_highlight_repair_enabled: false,
            optics: ffi::FfiOpticsSettings {
                enabled: true,
                correct_distortion: true,
                correct_tca: true,
                correct_vignetting: true,
                automatic_scale: true,
                manual_distortion: 0,
                manual_tca_red_cyan: 0,
                manual_tca_blue_yellow: 0,
                manual_vignetting_amount: 0,
                manual_vignetting_midpoint: 50,
                camera_profile_maker: String::new(),
                camera_profile_model: String::new(),
                lens_profile_maker: String::new(),
                lens_profile_model: String::new(),
            },
            raw_ai_denoise_present: false,
            raw_ai_denoise_enabled: false,
            raw_ai_denoise_bypassed: false,
            raw_ai_denoise_model: 0,
            raw_ai_denoise_amount_percent: 100,
            raw_white_balance_mode: 0,
            temperature_kelvin: 5_500,
            tint: 0,
            as_shot_white_balance_available: false,
            as_shot_temperature_kelvin: 5_500,
            as_shot_tint: 0,
        },
        grade_nodes: vec![created.clone()],
        retouch_spots: Vec::new(),
        retouch_strokes: Vec::new(),
        retouch_enabled: true,
        image_completions: Vec::new(),
        image_completion_enabled: true,
        liquify_enabled: false,
        liquify_strokes: Vec::new(),
        geometry: ffi::FfiPhotoGeometry {
            present: false,
            enabled: true,
            crop_left: 0.0,
            crop_top: 0.0,
            crop_right: 1.0,
            crop_bottom: 1.0,
            quarter_turn: 0,
            straighten_degrees: 0.0,
            perspective_vertical: 0.0,
            perspective_horizontal: 0.0,
            flip_horizontal: false,
            flip_vertical: false,
        },
    })
    .expect("decode freshly allocated Grade Node");
    let tone_curve_slot = decoded.grade_nodes[0]
        .recipe_v1_identity
        .oklab_lightness_curve_render_op_id
        .as_uuid();
    assert_eq!(tone_curve_slot.get_version_num(), 8);
    assert_eq!(tone_curve_slot.get_variant(), uuid::Variant::RFC4122);
    for value in [
        &created.selective_tone_render_op_id,
        &created.perceptual_color_render_op_id,
        &created.sharpen_render_op_id,
    ] {
        let id = Uuid::parse_str(value).expect("deterministic fine-edit slot UUID");
        assert_eq!(id.get_version_num(), 8);
        assert_eq!(id.get_variant(), uuid::Variant::RFC4122);
    }
    assert!(created.fine.oklab_lightness_curve_points.is_empty());

    let incoming = ffi::FfiEditSettings {
        foundation: ffi::FfiPhotoFoundationSettings {
            enabled: true,
            raw_highlight_repair_enabled: true,
            optics: ffi::FfiOpticsSettings {
                enabled: true,
                correct_distortion: false,
                correct_tca: true,
                correct_vignetting: false,
                automatic_scale: true,
                manual_distortion: 0,
                manual_tca_red_cyan: 0,
                manual_tca_blue_yellow: 0,
                manual_vignetting_amount: 0,
                manual_vignetting_midpoint: 50,
                camera_profile_maker: "Pentax".to_owned(),
                camera_profile_model: "K10D".to_owned(),
                lens_profile_maker: "smc Pentax".to_owned(),
                lens_profile_model: "DA 35mm".to_owned(),
            },
            raw_ai_denoise_present: true,
            raw_ai_denoise_enabled: true,
            raw_ai_denoise_bypassed: false,
            raw_ai_denoise_model: 0,
            raw_ai_denoise_amount_percent: 100,
            raw_white_balance_mode: 1,
            temperature_kelvin: 6_200,
            tint: -8,
            as_shot_white_balance_available: true,
            as_shot_temperature_kelvin: 5_150,
            as_shot_tint: 6,
        },
        grade_nodes: vec![created],
        retouch_spots: Vec::new(),
        retouch_strokes: Vec::new(),
        retouch_enabled: true,
        image_completions: Vec::new(),
        image_completion_enabled: true,
        liquify_enabled: false,
        liquify_strokes: Vec::new(),
        geometry: ffi::FfiPhotoGeometry {
            present: true,
            enabled: true,
            crop_left: 0.0,
            crop_top: 0.0,
            crop_right: 1.0,
            crop_bottom: 1.0,
            quarter_turn: 0,
            straighten_degrees: 0.0,
            perspective_vertical: 0.35,
            perspective_horizontal: -0.2,
            flip_horizontal: false,
            flip_vertical: false,
        },
    };
    let decoded = decode_grade_stack_draft_recipe_v1(&incoming).expect("decode Grade Stack");
    let outgoing = encode_grade_stack_draft_recipe_v1(decoded).expect("encode Grade Stack");
    assert_eq!(outgoing.grade_nodes.len(), 1);
    assert!(outgoing.foundation.raw_highlight_repair_enabled);
    assert_eq!(
        outgoing.grade_nodes[0].grade_node_id,
        incoming.grade_nodes[0].grade_node_id
    );
    assert_eq!(
        outgoing.grade_nodes[0].selective_tone_render_op_id,
        incoming.grade_nodes[0].selective_tone_render_op_id
    );
    assert_eq!(
        outgoing.grade_nodes[0].perceptual_color_render_op_id,
        incoming.grade_nodes[0].perceptual_color_render_op_id
    );
    assert_eq!(
        outgoing.grade_nodes[0].sharpen_render_op_id,
        incoming.grade_nodes[0].sharpen_render_op_id
    );
    assert_eq!(outgoing.grade_nodes[0].label, "Portrait foundation");
    assert!(outgoing.foundation.optics.enabled);
    assert!(!outgoing.foundation.optics.correct_distortion);
    assert!(outgoing.foundation.optics.correct_tca);
    assert!(!outgoing.foundation.optics.correct_vignetting);
    assert!(outgoing.foundation.optics.automatic_scale);
    assert_eq!(outgoing.foundation.optics.camera_profile_model, "K10D");
    assert_eq!(outgoing.foundation.optics.lens_profile_model, "DA 35mm");
    assert!(outgoing.foundation.raw_ai_denoise_enabled);
    assert_eq!(outgoing.foundation.raw_ai_denoise_model, 0);
    assert_eq!(outgoing.foundation.raw_white_balance_mode, 1);
    assert_eq!(outgoing.foundation.temperature_kelvin, 6_200);
    assert_eq!(outgoing.foundation.tint, -8);
    assert!(outgoing.geometry.present);
    assert_eq!(outgoing.geometry.perspective_vertical, 0.35);
    assert_eq!(outgoing.geometry.perspective_horizontal, -0.2);
}

#[test]
fn explicit_fine_edit_render_op_ids_survive_recipe_ffi_recipe_round_trip() {
    let mut grade_node = GradeNodeDraft::neutral("Imported fine-edit identities");
    let selective_tone_id =
        NodeId::from_uuid(Uuid::from_u128(0x11111111_2222_4333_8444_555555555555));
    let perceptual_color_id =
        NodeId::from_uuid(Uuid::from_u128(0xaaaaaaaa_bbbb_4ccc_8ddd_eeeeeeeeeeee));
    let sharpen_id = NodeId::from_uuid(Uuid::from_u128(0x01234567_89ab_4cde_8fed_cba987654321));
    assert_ne!(
        selective_tone_id,
        recipe_v1_selective_tone_render_op_id(grade_node.recipe_v1_identity.grade_node_id)
    );
    assert_ne!(
        perceptual_color_id,
        recipe_v1_perceptual_color_render_op_id(grade_node.recipe_v1_identity.grade_node_id)
    );
    assert_ne!(
        sharpen_id,
        recipe_v1_sharpen_render_op_id(grade_node.recipe_v1_identity.grade_node_id)
    );
    grade_node.recipe_v1_identity.selective_tone_render_op_id = selective_tone_id;
    grade_node.recipe_v1_identity.perceptual_color_render_op_id = perceptual_color_id;
    grade_node.recipe_v1_identity.sharpen_render_op_id = sharpen_id;
    grade_node.fine.selective_tone.highlights = -0.35;
    grade_node.fine.perceptual_color.vibrance = 0.42;

    let snapshot = grade_stack_recipe_v1_snapshot(
        &GradeStackDraft {
            raw_ai_denoise: shadow_domain::RawFoundationDenoise::disabled(),
            foundation: PhotoFoundationNode::default(),
            grade_nodes: vec![grade_node],
            retouch_spots: Vec::new(),
            retouch_strokes: Vec::new(),
            retouch_enabled: true,
            image_completions: Vec::new(),
            image_completion_enabled: true,
            liquify: None,
            canvas: PhotoCanvasNode::identity(),
        },
        None,
    )
    .expect("Recipe with externally allocated fine-edit identities");
    let recipe_decoded = decode_grade_stack_draft_from_recipe_v1_snapshot(&snapshot)
        .expect("decode Recipe before crossing Qt FFI");
    let ffi_settings =
        encode_grade_stack_draft_recipe_v1(recipe_decoded).expect("encode Grade Stack");
    assert_eq!(
        ffi_settings.grade_nodes[0].selective_tone_render_op_id,
        selective_tone_id.to_string()
    );
    assert_eq!(
        ffi_settings.grade_nodes[0].perceptual_color_render_op_id,
        perceptual_color_id.to_string()
    );
    assert_eq!(
        ffi_settings.grade_nodes[0].sharpen_render_op_id,
        sharpen_id.to_string()
    );

    let ffi_decoded = decode_grade_stack_draft_recipe_v1(&ffi_settings)
        .expect("decode Grade Stack after Qt FFI round trip");
    assert_eq!(
        ffi_decoded.recipe_v1_identity.selective_tone_render_op_id,
        selective_tone_id
    );
    assert_eq!(
        ffi_decoded.recipe_v1_identity.perceptual_color_render_op_id,
        perceptual_color_id
    );
    assert_eq!(
        ffi_decoded.recipe_v1_identity.sharpen_render_op_id,
        sharpen_id
    );
    let rebuilt = grade_stack_recipe_v1_snapshot(&ffi_decoded, Some(&snapshot))
        .expect("save the FFI round-tripped Recipe");
    assert_eq!(rebuilt, snapshot);
}

#[test]
fn curve_less_recipe_derives_the_same_oklab_curve_slot_on_repeated_reads() {
    let snapshot = grade_stack_recipe_v1_snapshot(&GradeStackDraft::default(), None)
        .expect("curve-less Basic Recipe");
    let first =
        decode_grade_stack_draft_from_recipe_v1_snapshot(&snapshot).expect("first Recipe read");
    let second =
        decode_grade_stack_draft_from_recipe_v1_snapshot(&snapshot).expect("second Recipe read");
    let grade_node_id = snapshot.layers()[0].id();

    assert_eq!(
        first.recipe_v1_identity.oklab_lightness_curve_render_op_id,
        second.recipe_v1_identity.oklab_lightness_curve_render_op_id
    );
    assert_eq!(
        first.recipe_v1_identity.oklab_lightness_curve_render_op_id,
        recipe_v1_oklab_lightness_tone_curve_render_op_id(grade_node_id)
    );
    assert_eq!(
        first
            .recipe_v1_identity
            .oklab_lightness_curve_render_op_id
            .as_uuid()
            .get_version_num(),
        8
    );
    assert_eq!(
        first
            .recipe_v1_identity
            .oklab_lightness_curve_render_op_id
            .as_uuid()
            .get_variant(),
        uuid::Variant::RFC4122
    );

    let reserved_id = first.recipe_v1_identity.oklab_lightness_curve_render_op_id;
    let mut curved_settings = first;
    curved_settings.fine.oklab_lightness_curve = Some(OklabLightnessToneCurve {
        lightness: vec![
            ToneCurvePoint { x: 0.0, y: 0.0 },
            ToneCurvePoint { x: 0.5, y: 0.7 },
            ToneCurvePoint { x: 1.0, y: 1.0 },
        ],
    });
    let curved = grade_stack_recipe_v1_snapshot(&curved_settings, Some(&snapshot))
        .expect("insert Oklab curve using the derived slot");
    assert_eq!(
        single_grade_node_recipe_v1_render_ops(&curved)
            .expect("curved render operations")
            .oklab_lightness_curve
            .expect("Oklab curve operation")
            .id(),
        reserved_id
    );

    let mut reset_settings =
        decode_grade_stack_draft_from_recipe_v1_snapshot(&curved).expect("curved Grade Stack");
    assert_eq!(
        reset_settings
            .recipe_v1_identity
            .oklab_lightness_curve_render_op_id,
        reserved_id
    );
    reset_settings.fine.oklab_lightness_curve = None;
    let reset = grade_stack_recipe_v1_snapshot(&reset_settings, Some(&curved))
        .expect("remove Oklab curve without rewriting the reserved slot");
    assert!(
        single_grade_node_recipe_v1_render_ops(&reset)
            .expect("reset render operations")
            .oklab_lightness_curve
            .is_none()
    );
}

#[test]
fn current_single_layer_snapshot_round_trips_without_identity_or_label_loss() {
    let mut grade_node = GradeNodeDraft::neutral("Custom grade");
    grade_node.basic.exposure_stops = 0.75;
    let grade_stack = GradeStackDraft {
        raw_ai_denoise: shadow_domain::RawFoundationDenoise::disabled(),
        foundation: PhotoFoundationNode::default(),
        grade_nodes: vec![grade_node],
        retouch_spots: Vec::new(),
        retouch_strokes: Vec::new(),
        retouch_enabled: true,
        image_completions: Vec::new(),
        image_completion_enabled: true,
        liquify: None,
        canvas: PhotoCanvasNode::identity(),
    };
    let snapshot = grade_stack_recipe_v1_snapshot(&grade_stack, None).expect("current snapshot");
    let original_identity = single_grade_node_recipe_v1_identity(&snapshot)
        .expect("read identity")
        .expect("one layer");

    let decoded = decode_grade_stack_draft_from_recipe_v1_snapshot(&snapshot)
        .expect("decode current snapshot");
    let rebuilt =
        grade_stack_recipe_v1_snapshot(&decoded, Some(&snapshot)).expect("rebuild snapshot");
    let rebuilt_identity = single_grade_node_recipe_v1_identity(&rebuilt)
        .expect("read rebuilt identity")
        .expect("one layer");

    assert_eq!(rebuilt, snapshot);
    assert_eq!(rebuilt_identity, original_identity);
    assert_eq!(rebuilt.layers()[0].label(), "Custom grade");
}
