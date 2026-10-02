use std::{fs, path::PathBuf};

use super::{folder, folder_with_cache};

struct ScanFixture(PathBuf);

impl ScanFixture {
    fn new() -> Self {
        let root = std::env::temp_dir().join(format!("shadow-cli-scan-{}", uuid::Uuid::now_v7()));
        fs::create_dir_all(&root).expect("create scan fixture");
        Self(root)
    }

    fn run(&self, input: &str, with_cache: bool) -> anyhow::Result<()> {
        let catalog = self.0.join("catalog.sqlite");
        let cache = self.0.join("cache");
        let input = self.0.join(input);
        if with_cache {
            folder_with_cache(
                catalog.to_str().expect("catalog path"),
                cache.to_str().expect("cache path"),
                input.to_str().expect("input path"),
            )
        } else {
            folder(
                catalog.to_str().expect("catalog path"),
                input.to_str().expect("input path"),
            )
        }
    }
}

impl Drop for ScanFixture {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

#[test]
fn missing_scan_root_is_rejected_before_catalog_or_cache_creation() {
    for with_cache in [false, true] {
        let fixture = ScanFixture::new();
        assert!(fixture.run("missing", with_cache).is_err());
        assert!(!fixture.0.join("catalog.sqlite").exists());
        assert!(!fixture.0.join("cache").exists());
    }
}

#[test]
fn regular_file_scan_root_is_rejected_before_catalog_or_cache_creation() {
    for with_cache in [false, true] {
        let fixture = ScanFixture::new();
        fs::write(fixture.0.join("input.jpg"), b"not a directory").expect("create input file");
        assert!(fixture.run("input.jpg", with_cache).is_err());
        assert!(!fixture.0.join("catalog.sqlite").exists());
        assert!(!fixture.0.join("cache").exists());
    }
}

#[test]
fn existing_empty_scan_root_succeeds() {
    for with_cache in [false, true] {
        let fixture = ScanFixture::new();
        fs::create_dir(fixture.0.join("empty")).expect("create empty input directory");
        fixture
            .run("empty", with_cache)
            .expect("scan empty directory");
        assert!(fixture.0.join("catalog.sqlite").is_file());
    }
}
