use super::*;
use crate::recipe::MaskDefinition;

fn reference() -> ManagedRasterMask {
    let content_blake3 = "ab".repeat(32);
    ManagedRasterMask::new(
        format!(
            "objects/v1/b3/{}/{}",
            &content_blake3[..2],
            &content_blake3[2..]
        ),
        1,
        content_blake3,
        8,
        4,
        2,
        6000,
        4000,
        RasterMaskEncoding::Gray8Unorm,
    )
    .expect("managed raster mask")
}

#[test]
fn managed_raster_mask_round_trips_as_an_immutable_recipe_definition() {
    let definition = MaskDefinition::managed_raster(reference(), true).expect("raster mask");
    let encoded = serde_json::to_string(&definition).expect("serialize raster mask");
    assert_eq!(
        encoded,
        concat!(
            r#"{"kind":"managed_raster","raster":{"contract_version":1,"#,
            r#""store_object_id":"objects/v1/b3/ab/"#,
            r#"ababababababababababababababababababababababababababababababab","#,
            r#""storage_revision":1,"content_blake3":"#,
            r#""abababababababababababababababababababababababababababababababab","#,
            r#""byte_len":8,"raster_width":4,"raster_height":2,"#,
            r#""coordinate_width":6000,"coordinate_height":4000,"#,
            r#""encoding":"gray8_unorm"},"expansion_percent":0,"#,
            r#""feather_percent":0,"invert":true}"#
        )
    );
    let decoded: MaskDefinition = serde_json::from_str(&encoded).expect("deserialize raster mask");
    assert_eq!(decoded, definition);
}

#[test]
fn managed_raster_refinement_round_trips_without_changing_raster_identity() {
    let definition = MaskDefinition::managed_raster_with_refinement(reference(), -35, 24, false)
        .expect("refined raster mask");
    let encoded = serde_json::to_string(&definition).expect("serialize refined mask");
    assert!(encoded.contains(r#""expansion_percent":-35"#));
    assert!(encoded.contains(r#""feather_percent":24"#));

    let decoded: MaskDefinition = serde_json::from_str(&encoded).expect("deserialize refined mask");
    assert_eq!(decoded, definition);
}

#[test]
fn managed_raster_refinement_rejects_out_of_range_percentages() {
    assert_eq!(
        MaskDefinition::managed_raster_with_refinement(reference(), -101, 0, false),
        Err(RecipeValidationError::InvalidManagedRasterMaskExpansion(
            -101
        ))
    );
    assert_eq!(
        MaskDefinition::managed_raster_with_refinement(reference(), 0, 101, false),
        Err(RecipeValidationError::InvalidManagedRasterMaskFeather(101))
    );
}

#[test]
fn managed_raster_mask_rejects_substituted_storage_and_byte_shape() {
    let mut wrong_object = reference();
    wrong_object.store_object_id = "objects/v1/b3/ab/substituted".into();
    assert_eq!(
        wrong_object.validate(),
        Err(RecipeValidationError::InvalidManagedRasterMaskStoreObjectId)
    );

    let hash = "cd".repeat(32);
    assert_eq!(
        ManagedRasterMask::new(
            format!("objects/v1/b3/{}/{}", &hash[..2], &hash[2..]),
            1,
            hash,
            7,
            4,
            2,
            6000,
            4000,
            RasterMaskEncoding::Gray8Unorm,
        ),
        Err(RecipeValidationError::ManagedRasterMaskByteLengthMismatch {
            expected: 8,
            actual: 7,
        })
    );
}
