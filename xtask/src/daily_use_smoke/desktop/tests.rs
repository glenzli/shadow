use super::*;

#[test]
fn optimized_desktop_bundle_is_outside_the_shared_source_root() {
    let root = Path::new("/tmp/shadow-workspace");
    assert_eq!(
        release_executable_from(root, None).expect("derive desktop app path"),
        PathBuf::from(
            "/tmp/.shadow-local-build/desktop-release/apps/desktop/Shadow.app/Contents/MacOS/Shadow"
        )
    );
}

#[test]
fn explicit_desktop_bundle_supports_a_task_private_external_build() {
    let root = Path::new("/tmp/shadow-workspace");
    assert_eq!(
        release_executable_from(
            root,
            Some("/tmp/shadow-acceptance/apps/desktop/Shadow.app/Contents/MacOS/Shadow".into())
        )
        .expect("accept external desktop app path"),
        PathBuf::from("/tmp/shadow-acceptance/apps/desktop/Shadow.app/Contents/MacOS/Shadow")
    );
    assert!(
        release_executable_from(root, Some("relative/Shadow".into())).is_err(),
        "relative overrides would make acceptance depend on the caller's current directory"
    );
    assert!(
        release_executable_from(root, Some("/tmp/shadow-workspace/build/Shadow".into())).is_err(),
        "acceptance build products may not enter the shared source tree"
    );
}
