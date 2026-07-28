use crate::writer::CatalogActor;

#[test]
fn actor_persists_immutable_export_preset_revisions() {
    let actor = CatalogActor::spawn_in_memory().expect("spawn catalog actor");
    let handle = actor.handle();
    let first = handle
        .create_export_preset("Actor JPEG", r#"{"format":"jpeg","quality":80}"#, 1)
        .expect("create preset through actor");
    let preset_id = first.preset_id;
    let second = handle
        .revise_export_preset(preset_id, r#"{"format":"jpeg","quality":90}"#, 2)
        .expect("revise preset through actor");

    assert_eq!(
        handle
            .export_preset_revisions(preset_id)
            .expect("read revisions through actor"),
        vec![second, first]
    );
    let presets = handle.export_presets().expect("list presets through actor");
    assert_eq!(presets.len(), 1);
    assert_eq!(presets[0].id, preset_id);
    assert_eq!(presets[0].name, "Actor JPEG");
    actor.shutdown().expect("shutdown actor");
}
