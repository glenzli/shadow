use shadow_domain::{EntityId, PhotoVariantId, RecipeId};

use super::{
    super::{
        ActivatePhotoVariant, CommitRecipe, CreatePhotoVariant, RecipeRefExpectation,
        RecipeRefKind, RecipeRefTarget, RemovePhotoVariant, RenamePhotoVariant,
    },
    recipe_fixtures::{catalog_with_photo, commit},
};
use crate::CatalogError;

#[allow(clippy::too_many_lines)] // One transaction sequence proves Variant head isolation.
#[test]
fn variants_share_the_photo_source_but_own_independent_recipe_heads() {
    let (mut catalog, photo_id) = catalog_with_photo("/photos/variants.dng");
    let initial = catalog.photo_variants(photo_id).expect("default Variant");
    assert_eq!(initial.len(), 1);
    assert!(initial[0].is_default);
    assert!(initial[0].is_active);
    assert!(initial[0].name.is_empty());
    assert_eq!(initial[0].head_commit_id, None);

    let recipe_id = RecipeId::new_v7();
    let natural = commit(recipe_id, Vec::new(), "Natural", 100);
    catalog
        .commit_recipe(&CommitRecipe {
            photo_id,
            commit: natural.clone(),
            update_refs: vec![RecipeRefTarget {
                name: "working".into(),
                kind: RecipeRefKind::Working,
                expectation: Some(RecipeRefExpectation::Missing),
            }],
        })
        .expect("save default Variant");
    assert_eq!(
        catalog.photo_variants(photo_id).expect("default head")[0].head_commit_id,
        Some(natural.id())
    );

    let dramatic_id = PhotoVariantId::new_v7();
    let dramatic = catalog
        .create_photo_variant(&CreatePhotoVariant {
            id: dramatic_id,
            photo_id,
            name: "Dramatic".into(),
            source_commit_id: Some(natural.id()),
            activate: true,
            created_at_ms: 200,
        })
        .expect("clone and activate Variant");
    assert!(dramatic.is_active);
    assert_eq!(dramatic.head_commit_id, Some(natural.id()));
    assert_eq!(
        catalog
            .recipe_ref(photo_id, "working")
            .expect("working ref")
            .expect("working head")
            .commit_id,
        natural.id()
    );

    let warm = commit(recipe_id, vec![natural.id()], "Warm", 300);
    catalog
        .commit_recipe(&CommitRecipe {
            photo_id,
            commit: warm.clone(),
            update_refs: vec![RecipeRefTarget {
                name: "working".into(),
                kind: RecipeRefKind::Working,
                expectation: Some(RecipeRefExpectation::At(natural.id())),
            }],
        })
        .expect("advance active Variant");
    let variants = catalog.photo_variants(photo_id).expect("independent heads");
    assert_eq!(variants[0].head_commit_id, Some(natural.id()));
    assert_eq!(variants[1].head_commit_id, Some(warm.id()));
    assert!(variants[1].is_active);

    assert!(matches!(
        catalog.remove_photo_variant(&RemovePhotoVariant {
            id: dramatic_id,
            photo_id,
        }),
        Err(CatalogError::CannotRemoveActivePhotoVariant)
    ));
    catalog
        .rename_photo_variant(&RenamePhotoVariant {
            id: dramatic_id,
            photo_id,
            name: "Warm editorial".into(),
            updated_at_ms: 350,
        })
        .expect("rename Variant");
    catalog
        .activate_photo_variant(&ActivatePhotoVariant {
            id: initial[0].id,
            photo_id,
            updated_at_ms: 400,
        })
        .expect("return to original Variant");
    assert_eq!(
        catalog
            .recipe_ref(photo_id, "working")
            .expect("working ref")
            .expect("restored head")
            .commit_id,
        natural.id()
    );
    catalog
        .remove_photo_variant(&RemovePhotoVariant {
            id: dramatic_id,
            photo_id,
        })
        .expect("remove inactive Variant");
    assert_eq!(
        catalog.photo_variants(photo_id).expect("remaining").len(),
        1
    );
}

#[test]
fn a_neutral_variant_removes_the_working_projection_and_recipe_reset_preserves_variants() {
    let (mut catalog, photo_id) = catalog_with_photo("/photos/neutral-variant.dng");
    let recipe_id = RecipeId::new_v7();
    let adjusted = commit(recipe_id, Vec::new(), "Adjusted", 100);
    catalog
        .commit_recipe(&CommitRecipe {
            photo_id,
            commit: adjusted.clone(),
            update_refs: vec![RecipeRefTarget {
                name: "working".into(),
                kind: RecipeRefKind::Working,
                expectation: Some(RecipeRefExpectation::Missing),
            }],
        })
        .expect("save original Variant");
    let neutral_id = PhotoVariantId::new_v7();
    catalog
        .create_photo_variant(&CreatePhotoVariant {
            id: neutral_id,
            photo_id,
            name: "Neutral".into(),
            source_commit_id: None,
            activate: true,
            created_at_ms: 200,
        })
        .expect("create neutral Variant");
    assert!(
        catalog
            .recipe_ref(photo_id, "working")
            .expect("read neutral working projection")
            .is_none()
    );

    catalog
        .activate_photo_variant(&ActivatePhotoVariant {
            id: PhotoVariantId::from_uuid(photo_id.as_uuid()),
            photo_id,
            updated_at_ms: 300,
        })
        .expect("restore adjusted original");
    catalog
        .discard_recipe_history(photo_id)
        .expect("discard incompatible Recipe history");
    let variants = catalog
        .photo_variants(photo_id)
        .expect("preserved Variants");
    assert_eq!(variants.len(), 2);
    assert!(
        variants
            .iter()
            .all(|variant| variant.head_commit_id.is_none())
    );
    assert!(
        catalog
            .recipe_ref(photo_id, "working")
            .expect("working ref after reset")
            .is_none()
    );
}

#[test]
fn variant_names_are_deliberately_user_facing_and_bounded() {
    let (mut catalog, photo_id) = catalog_with_photo("/photos/name.dng");
    assert!(matches!(
        catalog.create_photo_variant(&CreatePhotoVariant {
            id: PhotoVariantId::new_v7(),
            photo_id,
            name: "  padded  ".into(),
            source_commit_id: None,
            activate: false,
            created_at_ms: 1,
        }),
        Err(CatalogError::InvalidPhotoVariant(_))
    ));
}

#[test]
fn a_stale_editor_cannot_publish_into_a_newly_active_variant() {
    let (mut catalog, photo_id) = catalog_with_photo("/photos/stale-variant.dng");
    let original_id = PhotoVariantId::from_uuid(photo_id.as_uuid());
    let alternate_id = PhotoVariantId::new_v7();
    catalog
        .create_photo_variant(&CreatePhotoVariant {
            id: alternate_id,
            photo_id,
            name: "Alternate".into(),
            source_commit_id: None,
            activate: true,
            created_at_ms: 10,
        })
        .expect("activate alternate");
    catalog
        .activate_photo_variant(&ActivatePhotoVariant {
            id: original_id,
            photo_id,
            updated_at_ms: 20,
        })
        .expect("another editor activates original");

    let stale_commit = commit(RecipeId::new_v7(), Vec::new(), "Stale", 30);
    assert!(matches!(
        catalog.commit_recipe_for_variant(
            &CommitRecipe {
                photo_id,
                commit: stale_commit,
                update_refs: vec![RecipeRefTarget {
                    name: "working".into(),
                    kind: RecipeRefKind::Working,
                    expectation: Some(RecipeRefExpectation::Missing),
                }],
            },
            alternate_id,
        ),
        Err(CatalogError::PhotoVariantExpectationMismatch {
            expected,
            actual,
            ..
        }) if expected == alternate_id && actual == original_id
    ));
    assert!(
        catalog
            .recipe_commits(photo_id)
            .expect("no stale commit")
            .is_empty()
    );
}
