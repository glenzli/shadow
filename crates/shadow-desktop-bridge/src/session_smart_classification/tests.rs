use shadow_domain::EntityId;
use std::sync::atomic::Ordering;

#[test]
fn cancellation_is_session_local_and_cannot_cancel_a_newer_batch() {
    let root = std::env::temp_dir().join(format!(
        "shadow-classification-cancellation-{}",
        shadow_domain::PhotoId::new_v7()
    ));
    std::fs::create_dir_all(&root).unwrap();
    let open = |name: &str| {
        crate::open_desktop_session(
            root.join(name).join("catalog.sqlite").to_str().unwrap(),
            root.join(name).join("cache").to_str().unwrap(),
        )
        .unwrap()
    };
    let daily = open("daily");
    let debug = open("debug");
    let old = debug.begin_smart_classification().unwrap();
    let current = debug.begin_smart_classification().unwrap();
    debug.cancel_smart_classification(old);
    assert_eq!(
        debug.smart_classification_token.load(Ordering::Acquire),
        current
    );
    let daily_token = daily.begin_smart_classification().unwrap();
    debug.cancel_smart_classification(current);
    assert_ne!(
        debug.smart_classification_token.load(Ordering::Acquire),
        current
    );
    assert_eq!(
        daily.smart_classification_token.load(Ordering::Acquire),
        daily_token
    );
    debug
        .smart_classification_token
        .store(u64::MAX - 1, Ordering::Release);
    assert!(
        debug.begin_smart_classification().is_err(),
        "tokens never wrap or become uncancellable"
    );
    drop(debug);
    drop(daily);
    std::fs::remove_dir_all(root).unwrap();
}
