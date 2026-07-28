use super::*;

#[test]
fn entity_map_normalizes_order_and_rejects_duplicate_keys() {
    let first = EditObjectId::from_bytes([1; 32]);
    let second = EditObjectId::from_bytes([2; 32]);
    let map = EditEntityMapV1::new(vec![
        EditEntityEntryV1 {
            key: "photo/b".into(),
            value: second,
        },
        EditEntityEntryV1 {
            key: "photo/a".into(),
            value: first,
        },
    ])
    .unwrap();
    assert_eq!(
        map.entries()
            .iter()
            .map(|entry| entry.key.as_str())
            .collect::<Vec<_>>(),
        vec!["photo/a", "photo/b"]
    );

    assert!(matches!(
        EditEntityMapV1::new(vec![
            EditEntityEntryV1 {
                key: "photo/a".into(),
                value: first,
            },
            EditEntityEntryV1 {
                key: "photo/a".into(),
                value: second,
            },
        ]),
        Err(EditRepositoryError::DuplicateOrUnsortedEntityKey(key)) if key == "photo/a"
    ));
}

#[test]
fn entity_map_updates_and_diffs_are_sorted_and_semantic() {
    let first = EditObjectId::from_bytes([1; 32]);
    let second = EditObjectId::from_bytes([2; 32]);
    let third = EditObjectId::from_bytes([3; 32]);
    let before = EditEntityMapV1::new(vec![
        EditEntityEntryV1 {
            key: "photo/a".into(),
            value: first,
        },
        EditEntityEntryV1 {
            key: "photo/b".into(),
            value: second,
        },
    ])
    .unwrap();
    let after = before
        .without_entry("photo/a")
        .with_entry("photo/b", third)
        .unwrap()
        .with_entry("photo/c", first)
        .unwrap();
    assert_eq!(after.get("photo/b"), Some(third));
    assert_eq!(
        before.diff(&after),
        vec![
            EditEntityChangeV1 {
                key: "photo/a".into(),
                before: Some(first),
                after: None,
            },
            EditEntityChangeV1 {
                key: "photo/b".into(),
                before: Some(second),
                after: Some(third),
            },
            EditEntityChangeV1 {
                key: "photo/c".into(),
                before: None,
                after: Some(first),
            },
        ]
    );
}
