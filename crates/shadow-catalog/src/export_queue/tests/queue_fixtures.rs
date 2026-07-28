use rusqlite::params;
use shadow_domain::{
    AssetLocation, EntityId, Platform, RecipeCommitId, RecipeId, RepresentationKind,
};

use crate::{
    Catalog, EnqueueExportJob, ExportJobRecord, ExportSettingsSource, NewExportItem, RegisterAsset,
};

pub(super) const SNAPSHOT_DIGEST: [u8; 32] = [7; 32];

pub(super) fn output(path: &str) -> AssetLocation {
    AssetLocation::new(Platform::MacOs, path.as_bytes().to_vec(), path)
}

pub(super) fn seeded_item(catalog: &mut Catalog) -> NewExportItem {
    let registered = catalog
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/photos/export.NEF".to_vec(),
                "/photos/export.NEF",
            ),
            byte_len: 42,
            modified_at_ms: Some(1),
            now_ms: 1,
        })
        .expect("register source");
    let commit_id = RecipeCommitId::new_v7();
    let recipe_id = RecipeId::new_v7();
    catalog
        .connection
        .execute(
            "INSERT INTO recipe_commits(
                 id, photo_id, recipe_id, commit_json, snapshot_digest, created_at_ms
             ) VALUES (?1, ?2, ?3, '{}', ?4, 2)",
            params![
                commit_id.as_bytes().as_slice(),
                registered.photo_id.as_bytes().as_slice(),
                recipe_id.as_bytes().as_slice(),
                SNAPSHOT_DIGEST.as_slice(),
            ],
        )
        .expect("seed immutable recipe snapshot");
    NewExportItem {
        photo_id: registered.photo_id,
        representation_id: registered.representation_id,
        recipe_commit_id: commit_id,
        recipe_snapshot_digest: SNAPSHOT_DIGEST,
        edit_commit_id: None,
        source_identity_json: r#"{"scope":"whole_file","digest":"test"}"#.into(),
        render_plan_json: r#"{"quality":"full","provider":"libraw"}"#.into(),
        output: output("/exports/export.jpg"),
    }
}

pub(super) fn enqueue(catalog: &mut Catalog) -> ExportJobRecord {
    let item = seeded_item(catalog);
    enqueue_items(catalog, vec![item], 3)
}

pub(super) fn enqueue_items(
    catalog: &mut Catalog,
    items: Vec<NewExportItem>,
    now_ms: i64,
) -> ExportJobRecord {
    catalog
        .enqueue_export_job(&EnqueueExportJob {
            settings: ExportSettingsSource::InlineJson(r#"{"format":"jpeg","quality":90}"#.into()),
            items,
            now_ms,
        })
        .expect("enqueue export")
}
