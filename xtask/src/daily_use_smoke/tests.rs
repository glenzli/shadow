use super::*;

#[test]
fn utf8_tail_starts_at_a_character_boundary() {
    assert_eq!(utf8_tail("ab中文", 4), "文");
}

#[test]
fn optimized_desktop_bundle_is_outside_the_shared_source_root() {
    let root = Path::new("/tmp/shadow-workspace");
    assert_eq!(
        release_desktop_executable(root).expect("derive desktop app path"),
        PathBuf::from(
            "/tmp/.shadow-local-build/desktop-release/apps/desktop/Shadow.app/Contents/MacOS/Shadow"
        )
    );
}

#[test]
fn desktop_diagnostic_filter_rejects_binding_and_resource_failures() {
    let diagnostics = unexpected_desktop_diagnostic_lines(
        "qml: preview ready\n\
         qrc:/Shadow/qml/Main.qml:44: TypeError: Cannot read property 'x'\n\
         qrc:/Shadow/qml/Panel.qml:12: Binding loop detected for property \"width\"\n\
         module \"Shadow.Missing\" is not installed\n\
         QObject::connect: No such signal Controller::finished()\n",
    );
    assert_eq!(diagnostics.len(), 4);
    assert!(diagnostics.iter().any(|line| line.contains("TypeError:")));
    assert!(
        diagnostics
            .iter()
            .any(|line| line.contains("Binding loop detected"))
    );
    assert!(
        diagnostics
            .iter()
            .any(|line| line.contains("is not installed"))
    );
    assert!(
        diagnostics
            .iter()
            .any(|line| line.contains("QObject::connect:"))
    );
}

#[test]
fn desktop_diagnostic_filter_allows_normal_acceptance_output() {
    assert!(
        unexpected_desktop_diagnostic_lines(
            "QStandardPaths: XDG_RUNTIME_DIR not set, defaulting to '/tmp/runtime'\n\
             Grade Stack smoke passed with three persisted Grade Nodes\n\
             qt.qml.context: deprecated context property lookup enabled\n",
        )
        .is_empty()
    );
}
