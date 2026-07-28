use shadow_domain::{
    AssetLocation, EntityId, PhotoId, Platform, RecipeCommit, RecipeCommitId, RecipeId,
    RecipeSnapshot, RepresentationKind,
};

use crate::{Catalog, RegisterAsset};

pub(super) fn catalog_with_photo(path: &str) -> (Catalog, PhotoId) {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let registered = catalog
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(Platform::MacOs, path.as_bytes().to_vec(), path),
            byte_len: 1,
            modified_at_ms: Some(1),
            now_ms: 1,
        })
        .expect("register photo");
    (catalog, registered.photo_id)
}

pub(super) fn commit(
    recipe_id: RecipeId,
    parents: Vec<RecipeCommitId>,
    message: &str,
    created_at_ms: i64,
) -> RecipeCommit {
    RecipeCommit::new(
        RecipeCommitId::new_v7(),
        recipe_id,
        parents,
        RecipeSnapshot::empty(),
        Some(message.into()),
        created_at_ms,
    )
    .expect("valid commit")
}
