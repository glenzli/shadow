use crate::Catalog;

#[test]
fn preset_revisions_are_immutable_snapshots() {
    let mut catalog = Catalog::open_in_memory().expect("catalog");
    let first = catalog
        .create_export_preset("Web JPEG", r#"{"format":"jpeg","quality":80}"#, 1)
        .expect("create preset");
    let second = catalog
        .revise_export_preset(first.preset_id, r#"{"format":"jpeg","quality":92}"#, 2)
        .expect("revise preset");

    assert_eq!(first.revision, 1);
    assert_eq!(second.revision, 2);
    assert_ne!(first.settings_digest, second.settings_digest);
    let revisions = catalog
        .export_preset_revisions(first.preset_id)
        .expect("list revisions");
    assert_eq!(revisions, vec![second, first]);
}
