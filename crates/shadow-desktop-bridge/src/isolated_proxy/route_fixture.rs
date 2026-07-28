//! Test-only source/helper filesystem fixtures for isolated route identities.

use std::{fs, path::PathBuf};

use uuid::Uuid;

pub(super) fn safety_fixture(name: &str) -> (PathBuf, PathBuf, PathBuf) {
    let root = std::env::temp_dir().join(format!(
        "shadow-isolated-proxy-{name}-{}-{}",
        std::process::id(),
        Uuid::now_v7()
    ));
    fs::create_dir_all(&root).expect("create safety fixture root");
    let source = root.join("sample.raw");
    let helper = root.join("helper-binary");
    fs::write(&source, b"first source bytes").expect("write source fixture");
    fs::write(&helper, b"first helper bytes").expect("write helper fixture");
    (root, source, helper)
}
