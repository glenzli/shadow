use super::*;
use crate::recipe_v1::{GradeStackDraft, encode_grade_stack_draft_recipe_v1};
use shadow_catalog::RegisterAsset;
use shadow_domain::{AssetLocation, EntityId, Platform, RecipeCommitId, RepresentationKind};

struct Fixture {
    session: Box<DesktopSession>,
    root: std::path::PathBuf,
    photo: PhotoId,
    source: String,
}
impl Fixture {
    fn new() -> Self {
        let root =
            std::env::temp_dir().join(format!("shadow-auto-start-{}", RecipeCommitId::new_v7()));
        std::fs::create_dir_all(&root).unwrap();
        let session = crate::open_desktop_session(
            root.join("catalog.sqlite").to_str().unwrap(),
            root.join("cache").to_str().unwrap(),
        )
        .unwrap();
        let source = root.join("input.dng").to_string_lossy().into_owned();
        let registered = session
            .catalog
            .register_asset(&RegisterAsset {
                kind: RepresentationKind::OriginalRaw,
                location: AssetLocation::new(
                    Platform::MacOs,
                    source.as_bytes().to_vec(),
                    source.clone(),
                ),
                byte_len: 4096,
                modified_at_ms: Some(100),
                now_ms: 100,
            })
            .unwrap();
        Self {
            session,
            root,
            photo: registered.photo_id,
            source,
        }
    }
}
impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.root);
    }
}
fn settings() -> ffi::FfiEditSettings {
    encode_grade_stack_draft_recipe_v1(GradeStackDraft::default()).unwrap()
}
fn binding(settings: &ffi::FfiEditSettings) -> ffi::FfiAutoStartMask {
    ffi::FfiAutoStartMask {
        proposal_token: 1,
        generation: 7,
        node_id: settings.grade_nodes[0].grade_node_id.clone(),
    }
}
#[test]
fn bindings_reject_duplicate_targets_and_missing_tokens_before_apply() {
    let settings = settings();
    let valid = binding(&settings);
    validate_bindings(&settings, std::slice::from_ref(&valid)).unwrap();
    assert!(validate_bindings(&settings, &[valid.clone(), valid]).is_err());
    let mut missing = binding(&settings);
    missing.node_id = "missing".into();
    assert!(validate_bindings(&settings, &[missing]).is_err());
    let mut inverted = settings.clone();
    inverted.grade_nodes[0].local_mask_invert = true;
    assert!(validate_bindings(&inverted, &[binding(&inverted)]).is_err());
    let mut zero = binding(&settings);
    zero.proposal_token = 0;
    assert!(validate_bindings(&settings, &[zero]).is_err());
}
#[test]
fn applying_multiple_adjustments_is_one_commit_and_rejects_a_stale_head() {
    let f = Fixture::new();
    let mut settings = settings();
    let mut second = crate::recipe_v1::new_basic_grade_node("Skin").unwrap();
    second.basic.exposure_stops = 0.12;
    settings.grade_nodes[0].basic.exposure_stops = 0.3;
    settings.grade_nodes.push(second);
    let applied = f
        .session
        .apply_auto_start(&f.photo.to_string(), &f.source, "", "", &settings, &[])
        .unwrap();
    assert_eq!(applied.settings.grade_nodes.len(), 2);
    assert_eq!(f.session.catalog.recipe_commits(f.photo).unwrap().len(), 1);
    assert!(
        f.session
            .apply_auto_start(&f.photo.to_string(), &f.source, "", "", &settings, &[])
            .is_err()
    );
    assert_eq!(f.session.catalog.recipe_commits(f.photo).unwrap().len(), 1);
}
#[test]
fn transient_mask_plan_matches_persisted_mask_at_every_strength() {
    use crate::subject_mask_service::SubjectMaskProposalPreview;
    use shadow_ai::RasterExtent;
    use shadow_domain::{ManagedRasterMask, MaskDefinition, RasterMaskEncoding};
    let f = Fixture::new();
    let samples = vec![0_u8, 64, 128, 255];
    let digest = blake3::hash(&samples).to_hex().to_string();
    let object_id = format!("objects/v1/b3/{}/{}", &digest[..2], &digest[2..]);
    let path = f.root.join("derived-rasters").join(&object_id);
    std::fs::create_dir_all(path.parent().unwrap()).unwrap();
    std::fs::write(&path, &samples).unwrap();
    let raster = ManagedRasterMask::new(
        object_id,
        1,
        digest,
        4,
        2,
        2,
        6000,
        4000,
        RasterMaskEncoding::Gray8Unorm,
    )
    .unwrap();
    for opacity in [0.01, 0.5, 1.0] {
        let mut draft = GradeStackDraft::default();
        draft.grade_nodes[0].basic.exposure_stops = 0.2;
        draft.grade_nodes[0].opacity = shadow_domain::UnitInterval::new(opacity).unwrap();
        let settings = encode_grade_stack_draft_recipe_v1(draft.clone()).unwrap();
        let mask = binding(&settings);
        let request = ffi::FfiEditPreviewRequest {
            base_commit_id: String::new(),
            settings,
            render_token: 0,
            max_edge: 1024,
            jpeg_quality: 95,
            policy: ffi::FfiEditPreviewPolicy::Settled,
            use_working_recipe: true,
            mask_coverage_requested: false,
            mask_coverage_target_layer_index: 0,
            mask_coverage_component_requested: false,
            mask_coverage_target_component_index: 0,
            mask_selection_revision: 0,
        };
        let preview = SubjectMaskProposalPreview {
            generation: 7,
            raster_extent: RasterExtent::new(2, 2).unwrap(),
            coordinate_extent: RasterExtent::new(6000, 4000).unwrap(),
            encoding: SoftMaskEncoding::Gray8Unorm,
            samples: samples.clone(),
        };
        let transient = resolve_candidate_render(
            &f.session.catalog,
            &f.root.join("cache"),
            f.photo,
            &request,
            std::slice::from_ref(&mask),
            |_| Ok(preview.clone()),
        )
        .unwrap();
        let mut stale = preview.clone();
        stale.generation = 8;
        assert!(
            resolve_candidate_render(
                &f.session.catalog,
                &f.root.join("cache"),
                f.photo,
                &request,
                &[mask],
                |_| Ok(stale.clone())
            )
            .is_err()
        );
        draft.grade_nodes[0].local_mask =
            Some(MaskDefinition::managed_raster(raster.clone(), false).unwrap());
        let persisted_settings = encode_grade_stack_draft_recipe_v1(draft).unwrap();
        // Persist the source Recipe so opaque managed-mask references resolve normally.
        let stack = decode_grade_stack_draft_recipe_v1(&request.settings).unwrap();
        let mut persisted_stack = stack;
        persisted_stack.grade_nodes[0].local_mask =
            Some(MaskDefinition::managed_raster(raster.clone(), false).unwrap());
        let current = f
            .session
            .catalog
            .recipe_ref(f.photo, "working")
            .unwrap()
            .map(|r| r.commit_id.to_string())
            .unwrap_or_default();
        let applied = f
            .session
            .autosave_grade_stack_working_at(
                &f.photo.to_string(),
                &f.source,
                &current,
                &current,
                &persisted_stack,
                1000,
            )
            .unwrap();
        let durable = resolve_recipe_render(
            &f.session.catalog,
            &f.root.join("cache"),
            f.photo,
            &applied.working_commit_id,
            &persisted_settings,
            true,
        )
        .unwrap();
        assert_eq!(transient.plan.nodes, durable.plan.nodes);
        assert_eq!(transient.plan.geometry, durable.plan.geometry);
    }
}
