//! Grade-stack identity, shared-node, retouch, geometry, and Recipe v1 shape contracts.

use super::*;

#[test]
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
        grade_nodes: vec![created.clone()],
        retouch_spots: Vec::new(),
        retouch_strokes: Vec::new(),
        geometry: ffi::FfiPhotoGeometry {
            crop_left: 0.0,
            crop_top: 0.0,
            crop_right: 1.0,
            crop_bottom: 1.0,
            quarter_turn: 0,
            straighten_degrees: 0.0,
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
        grade_nodes: vec![created],
        retouch_spots: Vec::new(),
        retouch_strokes: Vec::new(),
        geometry: ffi::FfiPhotoGeometry {
            crop_left: 0.0,
            crop_top: 0.0,
            crop_right: 1.0,
            crop_bottom: 1.0,
            quarter_turn: 0,
            straighten_degrees: 0.0,
            flip_horizontal: false,
            flip_vertical: false,
        },
    };
    let decoded = decode_grade_stack_draft_recipe_v1(&incoming).expect("decode Grade Stack");
    let outgoing = encode_grade_stack_draft_recipe_v1(decoded);
    assert_eq!(outgoing.grade_nodes.len(), 1);
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
    assert!(outgoing.optics.enabled);
    assert!(!outgoing.optics.correct_distortion);
    assert!(outgoing.optics.correct_tca);
    assert!(!outgoing.optics.correct_vignetting);
    assert!(outgoing.optics.automatic_scale);
    assert_eq!(outgoing.optics.camera_profile_model, "K10D");
    assert_eq!(outgoing.optics.lens_profile_model, "DA 35mm");
}

#[test]
fn continuous_retouch_strokes_round_trip_through_desktop_ffi_and_recipe_v1() {
    let mut incoming = ffi_parameters(0.0, 1.0, [0.0; 2], 1.0);
    incoming.retouch_spots = vec![ffi::FfiRetouchSpot {
        center_x: 0.2,
        center_y: 0.25,
        radius_level_zero_pixels: 12,
        mode: 0,
        source_offset_x_radii: 0.0,
        source_offset_y_radii: 0.0,
        feather: 0.28,
    }];
    incoming.retouch_strokes = vec![ffi::FfiRetouchStroke {
        points: vec![
            ffi::FfiRetouchPoint { x: 0.3, y: 0.4 },
            ffi::FfiRetouchPoint { x: 0.55, y: 0.65 },
        ],
        radius_level_zero_pixels: 24,
        mode: 1,
        source_offset_x_radii: 1.25,
        source_offset_y_radii: -0.75,
        feather: 0.4,
    }];

    let draft = decode_grade_stack_draft_recipe_v1(&incoming)
        .expect("decode continuous retouch FFI payload");
    assert_eq!(draft.retouch_spots.len(), 1);
    assert_eq!(draft.retouch_strokes.len(), 1);
    let stroke = &draft.retouch_strokes[0];
    assert_eq!(stroke.points().len(), 2);
    assert_eq!(stroke.points()[0].x().get(), 0.3);
    assert_eq!(stroke.points()[1].y().get(), 0.65);
    assert_eq!(stroke.radius_level_zero_pixels(), 24);
    assert_eq!(stroke.mode(), RetouchMode::Clone);
    assert_eq!(stroke.source_offset_x_radii(), 1.25);
    assert_eq!(stroke.source_offset_y_radii(), -0.75);
    assert_eq!(stroke.feather().get(), 0.4);

    let snapshot =
        grade_stack_recipe_v1_snapshot(&draft, None).expect("persist continuous retouch stroke");
    assert_eq!(snapshot.retouch_spots().len(), 1);
    assert_eq!(snapshot.retouch_strokes(), draft.retouch_strokes.as_slice());
    let from_snapshot = decode_grade_stack_draft_from_recipe_v1_snapshot(&snapshot)
        .expect("read continuous retouch stroke");
    assert_eq!(from_snapshot.retouch_strokes, draft.retouch_strokes);

    let plan = compile_recipe_render_plan(&snapshot)
        .expect("compile continuous retouch stroke into render plan");
    let (targets, strokes) = plan
        .nodes
        .iter()
        .find_map(|node| match &node.operation {
            AdjustmentRenderOperation::SpotHeal { targets, strokes } => Some((targets, strokes)),
            _ => None,
        })
        .expect("photo-local retouch render node");
    assert_eq!(targets.len(), 1);
    assert_eq!(strokes.len(), 1);
    assert_eq!(strokes[0].points.len(), 2);
    assert_eq!(strokes[0].points[0].x, 0.3);
    assert_eq!(strokes[0].points[1].y, 0.65);
    assert_eq!(strokes[0].mode, 1);
    assert_eq!(strokes[0].source_offset_x_radii, 1.25);

    let outgoing = encode_grade_stack_draft_recipe_v1(draft);
    assert_eq!(outgoing.retouch_spots.len(), 1);
    assert_eq!(outgoing.retouch_strokes.len(), 1);
    assert_eq!(outgoing.retouch_strokes[0].points.len(), 2);
    assert_eq!(outgoing.retouch_strokes[0].mode, 1);
    assert_eq!(outgoing.retouch_strokes[0].source_offset_x_radii, 1.25);

    incoming.retouch_strokes[0].points.clear();
    let error = decode_grade_stack_draft_recipe_v1(&incoming)
        .expect_err("empty continuous retouch stroke must fail closed");
    assert!(error.to_string().contains("retouch stroke 0 is invalid"));
}

#[test]
fn shared_grade_node_heads_are_named_versioned_and_renderable() {
    let root = std::env::temp_dir().join(format!(
        "shadow-shared-grade-node-{}-{}",
        std::process::id(),
        LayerId::new_v7()
    ));
    let session = open_desktop_session(
        root.join("catalog.sqlite").to_str().expect("catalog path"),
        root.join("cache").to_str().expect("cache path"),
    )
    .expect("open desktop session");
    let mut local = new_basic_grade_node("Portrait").expect("local Grade Node");
    local.basic.exposure_stops = 0.35;

    let first = session
        .publish_shared_grade_node("Portrait foundation", &local)
        .expect("publish shared Grade Node");
    assert_eq!(first.revision_number, 1);
    assert_eq!(first.label, "Portrait foundation");
    assert_eq!(first.grade_node.shared_layer_id, first.layer_id);
    assert_eq!(first.grade_node.shared_revision_id, first.revision_id);
    assert_close(first.grade_node.basic.exposure_stops, 0.35);

    let mut update = first.grade_node.clone();
    update.basic.exposure_stops = 0.8;
    let second = session
        .publish_shared_grade_node("Portrait foundation", &update)
        .expect("publish second shared revision");
    assert_eq!(second.layer_id, first.layer_id);
    assert_ne!(second.revision_id, first.revision_id);
    assert_eq!(second.revision_number, 2);
    assert_close(second.grade_node.basic.exposure_stops, 0.8);

    let listed = session.shared_grade_nodes().expect("list shared heads");
    assert_eq!(listed.len(), 1);
    assert_eq!(listed[0].revision_id, second.revision_id);
    assert_eq!(listed[0].grade_node.shared_layer_id, first.layer_id);

    drop(session);
    std::fs::remove_dir_all(root).expect("remove shared Grade Node fixture");
}

#[test]
fn batch_application_links_latest_shared_revision_and_is_idempotent() {
    let (root, session, photo_id, source_path) = test_edit_session();
    let mut local = new_basic_grade_node("Shared contrast").expect("local Grade Node");
    local.basic.contrast_factor = 1.12;
    let shared = session
        .publish_shared_grade_node("Shared contrast", &local)
        .expect("publish shared Grade Node");
    let target = || ffi::FfiBatchPhotoTarget {
        photo_id: photo_id.clone(),
        source_path: source_path.clone(),
    };

    let first = session
        .apply_shared_grade_node_to_photos(&shared.layer_id, vec![target()])
        .expect("apply shared Grade Node");
    assert_eq!(first.requested, 1);
    assert_eq!(first.updated, 1);
    assert_eq!(first.unchanged, 0);
    assert_eq!(first.failed, 0);
    let state = session
        .photo_edit_state(&photo_id, &source_path)
        .expect("read linked edit");
    assert_eq!(state.settings.grade_nodes.len(), 2);
    assert_eq!(
        state.settings.grade_nodes[1].shared_layer_id,
        shared.layer_id
    );
    assert_eq!(
        state.settings.grade_nodes[1].shared_revision_id,
        shared.revision_id
    );

    let second = session
        .apply_shared_grade_node_to_photos(&shared.layer_id, vec![target()])
        .expect("reapply shared Grade Node");
    assert_eq!(second.updated, 0);
    assert_eq!(second.unchanged, 1);
    assert_eq!(second.failed, 0);

    drop(session);
    std::fs::remove_dir_all(root).expect("remove batch Grade Node fixture");
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
            optics: RecipeOpticsSettings::default(),
            grade_nodes: vec![grade_node],
            retouch_spots: Vec::new(),
            retouch_strokes: Vec::new(),
            geometry: PhotoGeometry::identity(),
        },
        None,
    )
    .expect("Recipe with externally allocated fine-edit identities");
    let recipe_decoded = decode_grade_stack_draft_from_recipe_v1_snapshot(&snapshot)
        .expect("decode Recipe before crossing Qt FFI");
    let ffi_settings = encode_grade_stack_draft_recipe_v1(recipe_decoded);
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
}

#[test]
fn current_single_layer_snapshot_round_trips_without_identity_or_label_loss() {
    let mut grade_node = GradeNodeDraft::neutral("Custom grade");
    grade_node.basic.exposure_stops = 0.75;
    let grade_stack = GradeStackDraft {
        optics: RecipeOpticsSettings::default(),
        grade_nodes: vec![grade_node],
        retouch_spots: Vec::new(),
        retouch_strokes: Vec::new(),
        geometry: PhotoGeometry::identity(),
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

#[test]
fn two_grade_nodes_compile_in_recipe_vector_order_with_namespaced_render_ops() {
    let mut grade_stack = GradeStackDraft::default();
    grade_stack.basic.exposure_stops = 0.5;
    let mut second = GradeNodeDraft::neutral("Second Basic");
    second.basic.exposure_stops = -1.25;
    grade_stack.grade_nodes.push(second);
    let first_grade_node_id = grade_stack.grade_nodes[0].recipe_v1_identity.grade_node_id;
    let second_grade_node_id = grade_stack.grade_nodes[1].recipe_v1_identity.grade_node_id;

    let snapshot = grade_stack_recipe_v1_snapshot(&grade_stack, None).expect("two-node snapshot");
    let plan = compile_recipe_render_plan(&snapshot).expect("compile two Grade Nodes");
    assert_eq!(plan.nodes.len(), 20);
    assert!(
        plan.nodes[..10]
            .iter()
            .all(|node| node.node_id.starts_with(&format!("{first_grade_node_id}/")))
    );
    assert!(plan.nodes[10..].iter().all(|node| {
        node.node_id
            .starts_with(&format!("{second_grade_node_id}/"))
    }));
    assert!(matches!(
        plan.nodes[1].operation,
        AdjustmentRenderOperation::Exposure { stops: 0.5 }
    ));
    assert!(matches!(
        plan.nodes[11].operation,
        AdjustmentRenderOperation::Exposure { stops: -1.25 }
    ));

    grade_stack.grade_nodes.reverse();
    let reversed = grade_stack_recipe_v1_snapshot(&grade_stack, None).expect("reordered snapshot");
    let reversed_plan = compile_recipe_render_plan(&reversed).expect("compile reordered stack");
    assert!(
        reversed_plan.nodes[0]
            .node_id
            .starts_with(&format!("{second_grade_node_id}/"))
    );
}

#[test]
fn recipe_v1_rejects_an_operation_level_mask_reference() {
    let valid = grade_stack_recipe_v1_snapshot(&GradeStackDraft::default(), None)
        .expect("current Grade Node Recipe");
    let [valid_layer] = valid.layers() else {
        panic!("fixture contains one Grade Node")
    };
    let LayerContent::Inline { graph: valid_graph } = valid_layer.content() else {
        panic!("fixture contains an inline Grade Node graph")
    };
    let mask = MaskRevision::new(
        MaskId::new_v7(),
        1,
        MaskCoordinateSpace::Original,
        MaskDefinition::radial_gradient(
            UnitInterval::new(0.5).expect("center x"),
            UnitInterval::new(0.5).expect("center y"),
            UnitInterval::new(0.25).expect("radius x"),
            UnitInterval::new(0.25).expect("radius y"),
            UnitInterval::new(0.25).expect("feather"),
            false,
        )
        .expect("mask definition"),
    )
    .expect("mask revision");
    let mask_reference = mask.reference();
    let nodes = valid_graph
        .nodes()
        .iter()
        .map(|node| {
            if node.operation().operation_id().as_str() != EXPOSURE_OPERATION_ID {
                return node.clone();
            }
            AdjustmentNode::new(
                node.id(),
                node.operation().clone(),
                node.inputs().to_vec(),
                node.parameters().clone(),
                Some(mask_reference),
            )
            .expect("a domain node can carry a generic mask reference")
        })
        .collect::<Vec<_>>();
    let graph = EditGraph::new(
        valid_graph.schema_version(),
        valid_graph.input_types().to_vec(),
        nodes,
        valid_graph.output_node(),
    )
    .expect("domain graph remains structurally valid");
    let operation_masked = RecipeSnapshot::new_with_input_settings_and_masks(
        valid.schema_version(),
        valid.input_settings().clone(),
        vec![mask],
        vec![
            LayerInstance::new(
                valid_layer.id(),
                valid_layer.label(),
                AdjustmentScope::Photo,
                LayerContent::Inline { graph },
                valid_layer.enabled(),
                valid_layer.opacity(),
                valid_layer.blend_mode(),
                None,
            )
            .expect("a domain layer may still contain a generic graph"),
        ],
    )
    .expect("domain Recipe remains structurally valid");

    assert!(
        decode_grade_stack_draft_from_recipe_v1_snapshot(&operation_masked)
            .expect_err("Desktop Recipe v1 only supports one layer-level node mask")
            .to_string()
            .contains("unsupported contract")
    );
    assert!(compile_recipe_render_plan(&operation_masked).is_err());
}

#[test]
fn grade_stack_rejects_cross_grade_node_render_op_identity_reuse() {
    let first = GradeNodeDraft::neutral("First Basic");
    let mut second = GradeNodeDraft::neutral("Second Basic");
    second.recipe_v1_identity.exposure_render_op_id =
        first.recipe_v1_identity.exposure_render_op_id;
    let invalid = GradeStackDraft {
        optics: RecipeOpticsSettings::default(),
        grade_nodes: vec![first.clone(), second.clone()],
        retouch_spots: Vec::new(),
        retouch_strokes: Vec::new(),
        geometry: PhotoGeometry::identity(),
    };

    let ffi_error =
        decode_grade_stack_draft_recipe_v1(&encode_grade_stack_draft_recipe_v1(invalid.clone()))
            .expect_err("FFI stack must reject a render-op id reused by another Grade Node");
    assert!(
        ffi_error
            .to_string()
            .contains("duplicate Recipe v1 render-op id")
    );

    // RecipeSnapshot currently scopes graph identity validation per layer,
    // so the desktop compiler must independently enforce the stack-wide
    // identity contract for externally persisted snapshots.
    let persisted = RecipeSnapshot::new(
        CURRENT_RECIPE_SCHEMA_VERSION,
        vec![
            encode_grade_node_as_recipe_v1_layer(&first).expect("first persisted Grade Node"),
            encode_grade_node_as_recipe_v1_layer(&second).expect("second persisted Grade Node"),
        ],
    )
    .expect("domain-valid graph-scoped identities");
    assert!(
        decode_grade_stack_draft_from_recipe_v1_snapshot(&persisted)
            .expect_err("persisted stack read must reject reused render-op identity")
            .to_string()
            .contains("duplicate Recipe v1 render-op id")
    );
    assert!(
        compile_recipe_render_plan(&persisted)
            .expect_err("compiler must reject reused render-op identity")
            .to_string()
            .contains("duplicate render-op id")
    );
}

#[test]
fn duplicate_add_delete_and_reorder_preserve_the_expected_identities() {
    let mut original = GradeStackDraft::default();
    original.fine.selective_tone.shadows = 0.2;
    original.fine.perceptual_color.vibrance = 0.35;
    let base = grade_stack_recipe_v1_snapshot(&original, None).expect("base snapshot");
    let duplicate = original.grade_nodes[0].duplicate();
    assert_eq!(duplicate.basic, original.grade_nodes[0].basic);
    assert_eq!(duplicate.fine, original.grade_nodes[0].fine);
    assert_ne!(
        duplicate.recipe_v1_identity.grade_node_id,
        original.grade_nodes[0].recipe_v1_identity.grade_node_id
    );
    assert!(
        duplicate
            .recipe_v1_identity
            .recipe_v1_render_op_id_values()
            .into_iter()
            .all(|id| !original.grade_nodes[0]
                .recipe_v1_identity
                .recipe_v1_render_op_id_values()
                .contains(&id))
    );

    let mut added_settings = original.clone();
    added_settings.grade_nodes.push(duplicate.clone());
    let added =
        grade_stack_recipe_v1_snapshot(&added_settings, Some(&base)).expect("added snapshot");
    let added_diff = diff_recipe_snapshots(&base, &added);
    assert_eq!(added_diff.added_layers().len(), 1);
    assert_eq!(
        added_diff.added_layers()[0].id(),
        duplicate.recipe_v1_identity.grade_node_id
    );

    let mut reordered_settings = added_settings.clone();
    reordered_settings.grade_nodes.swap(0, 1);
    let reordered = grade_stack_recipe_v1_snapshot(&reordered_settings, Some(&added))
        .expect("reordered snapshot");
    let reordered_diff = diff_recipe_snapshots(&added, &reordered);
    assert_eq!(reordered_diff.moved_layers().len(), 2);
    assert_eq!(
        reordered.layers()[0].id(),
        duplicate.recipe_v1_identity.grade_node_id
    );

    reordered_settings.grade_nodes.remove(0);
    let deleted = grade_stack_recipe_v1_snapshot(&reordered_settings, Some(&reordered))
        .expect("deleted snapshot");
    assert_eq!(deleted, base);
    assert_eq!(
        diff_recipe_snapshots(&reordered, &deleted)
            .removed_layers()
            .len(),
        1
    );
}

#[test]
fn template_rejects_retained_identity_rewrite_and_deleted_node_reuse() {
    let mut base_settings = GradeStackDraft::default();
    base_settings
        .grade_nodes
        .push(GradeNodeDraft::neutral("Second Basic"));
    let base = grade_stack_recipe_v1_snapshot(&base_settings, None).expect("two-node base");

    let mut rewritten = base_settings.clone();
    rewritten.grade_nodes[0]
        .recipe_v1_identity
        .exposure_render_op_id = NodeId::new_v7();
    assert!(
        grade_stack_recipe_v1_snapshot(&rewritten, Some(&base))
            .expect_err("retained Grade Node render-op identity rewrite must fail")
            .to_string()
            .contains("must preserve every stable Recipe v1 render-op identity")
    );

    let deleted_exposure_id = base_settings.grade_nodes[0]
        .recipe_v1_identity
        .exposure_render_op_id;
    let mut replacement = GradeNodeDraft::neutral("Replacement Basic");
    replacement.recipe_v1_identity.exposure_render_op_id = deleted_exposure_id;
    let replacement_settings = GradeStackDraft {
        optics: RecipeOpticsSettings::default(),
        grade_nodes: vec![base_settings.grade_nodes[1].clone(), replacement],
        retouch_spots: Vec::new(),
        retouch_strokes: Vec::new(),
        geometry: PhotoGeometry::identity(),
    };
    assert!(
        grade_stack_recipe_v1_snapshot(&replacement_settings, Some(&base))
            .expect_err("new Grade Node must not reuse a deleted base render-op identity")
            .to_string()
            .contains("reuses base Recipe v1 render-op id")
    );
}

#[test]
#[cfg(any())]
fn template_allows_tone_curve_add_and_remove_with_the_reserved_identity() {
    let neutral_settings = GradeStackDraft::default();
    let neutral = grade_stack_recipe_v1_snapshot(&neutral_settings, None).expect("neutral Recipe");
    let mut curved_settings =
        decode_grade_stack_draft_from_recipe_v1_snapshot(&neutral).expect("neutral Grade Stack");
    let reserved_id = curved_settings.recipe_v1_identity.tone_curve_render_op_id;
    curved_settings.tone_curve = Some(ToneCurveDraft::SmoothRgb(Box::default()));
    let curved = grade_stack_recipe_v1_snapshot(&curved_settings, Some(&neutral))
        .expect("insert Tone Curve using reserved identity");
    assert_eq!(
        single_grade_node_recipe_v1_render_ops(&curved)
            .expect("curved nodes")
            .tone_curve
            .expect("Tone Curve node")
            .id(),
        reserved_id
    );

    let mut reset_settings =
        decode_grade_stack_draft_from_recipe_v1_snapshot(&curved).expect("curved Grade Stack");
    assert_eq!(
        reset_settings.recipe_v1_identity.tone_curve_render_op_id,
        reserved_id
    );
    reset_settings.tone_curve = None;
    grade_stack_recipe_v1_snapshot(&reset_settings, Some(&curved))
        .expect("remove Tone Curve without rewriting its incoming identity");
}

#[test]
fn grade_stack_accepts_sixteen_grade_nodes_and_rejects_seventeen() {
    assert!(
        grade_stack_recipe_v1_snapshot(
            &GradeStackDraft {
                optics: RecipeOpticsSettings::default(),
                grade_nodes: Vec::new(),
                retouch_spots: Vec::new(),
                retouch_strokes: Vec::new(),
                geometry: PhotoGeometry::identity(),
            },
            None
        )
        .expect_err("an empty stack must fail closed")
        .to_string()
        .contains("1 through 16")
    );
    let sixteen = GradeStackDraft {
        optics: RecipeOpticsSettings::default(),
        grade_nodes: (0..MAX_GRADE_NODES)
            .map(|index| GradeNodeDraft::neutral(format!("Basic {index}")))
            .collect(),
        retouch_spots: Vec::new(),
        retouch_strokes: Vec::new(),
        geometry: PhotoGeometry::identity(),
    };
    let snapshot = grade_stack_recipe_v1_snapshot(&sixteen, None).expect("sixteen-node snapshot");
    assert_eq!(
        compile_recipe_render_plan(&snapshot).unwrap().nodes.len(),
        160
    );

    let mut seventeen = sixteen.clone();
    seventeen
        .grade_nodes
        .push(GradeNodeDraft::neutral("One too many"));
    let error = grade_stack_recipe_v1_snapshot(&seventeen, None)
        .expect_err("seventeen Grade Nodes must fail closed");
    assert!(error.to_string().contains("1 through 16"));

    let mut ffi_seventeen = encode_grade_stack_draft_recipe_v1(sixteen);
    ffi_seventeen
        .grade_nodes
        .push(new_basic_grade_node("One too many").unwrap());
    assert!(
        decode_grade_stack_draft_recipe_v1(&ffi_seventeen)
            .expect_err("FFI seventeen Grade Nodes must fail")
            .to_string()
            .contains("1 through 16")
    );
}

#[test]
fn photo_geometry_round_trips_without_becoming_a_grade_node() {
    let geometry = PhotoGeometry::new(
        UnitInterval::new(0.125).expect("crop left"),
        UnitInterval::new(0.25).expect("crop top"),
        UnitInterval::new(0.875).expect("crop right"),
        UnitInterval::new(0.75).expect("crop bottom"),
        PhotoQuarterTurn::Clockwise90,
        true,
        false,
    )
    .expect("valid photo-local geometry");
    let mut grade_stack = GradeStackDraft::default();
    grade_stack.geometry = geometry;

    let snapshot =
        grade_stack_recipe_v1_snapshot(&grade_stack, None).expect("persist photo geometry");
    assert_eq!(snapshot.geometry(), geometry);
    assert_eq!(snapshot.layers().len(), 1, "geometry is not a Grade Node");

    let plan = compile_recipe_render_plan(&snapshot).expect("compile photo geometry");
    assert_eq!(plan.geometry.crop_left, 0.125);
    assert_eq!(plan.geometry.crop_top, 0.25);
    assert_eq!(plan.geometry.crop_right, 0.875);
    assert_eq!(plan.geometry.crop_bottom, 0.75);
    assert_eq!(
        plan.geometry.quarter_turn,
        AdjustmentQuarterTurn::Clockwise90
    );
    assert!(plan.geometry.flip_horizontal);
    assert!(!plan.geometry.flip_vertical);

    let ffi = encode_grade_stack_draft_recipe_v1(grade_stack);
    assert_eq!(ffi.geometry.quarter_turn, 1);
    assert!(ffi.geometry.flip_horizontal);
    assert!(!ffi.geometry.flip_vertical);
    let decoded = decode_grade_stack_draft_recipe_v1(&ffi).expect("decode photo geometry");
    assert_eq!(decoded.geometry, geometry);
}
