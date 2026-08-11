use std::{fs, io::Write as _};

use uuid::Uuid;

use super::{PROTOCOL, open_staged_frame};

const SAMPLE_SHA256: &str = "fe836c2f2358e0e032c4fde74500d9a30d484e5d106098dac748dcd9b599cff2";

struct FixtureRoot(std::path::PathBuf);

impl FixtureRoot {
    fn new() -> Self {
        let path =
            std::env::temp_dir().join(format!("shadow-raw-frame-staging-test-{}", Uuid::now_v7()));
        fs::create_dir(&path).expect("fixture root");
        Self(path)
    }

    fn path(&self) -> &std::path::Path {
        &self.0
    }
}

impl Drop for FixtureRoot {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

fn write_fixture(root: &std::path::Path, extra: &str) -> std::path::PathBuf {
    let manifest_path = root.join("frame.shadowrawi");
    let sample_path = root.join("frame.shadowrawi.u16le");
    fs::write(&sample_path, [0_u8, 1, 2, 3].repeat(8)).expect("sample fixture");
    let manifest = format!(
        "{PROTOCOL} descriptor_contract=active-camera-colour-20260806.1 width=4 height=4 cfa=RGGB black=64,64,64,64 white=16383,16383,16383,16383 orientation=0 bits_per_sample=14 as_shot_neutral=1,1,1,1 camera_to_xyz_d50=1,0,0,0,1,0,0,0,1 xyz_to_camera_d65=- camera_to_linear_srgb_d65=- pending_dng_opcode_bytes=0,0,0 provider_id_hex=736861646f772e746573742e726177 provider_version_hex=7631 sample_bytes=32{extra}\n"
    );
    fs::write(&manifest_path, manifest).expect("manifest fixture");
    manifest_path
}

#[test]
fn opens_one_strict_owner_only_descriptor_and_exact_sample_handle() {
    let root = FixtureRoot::new();
    let manifest_path = write_fixture(root.path(), "");

    let (sample_path, mut sample, descriptor) = open_staged_frame(&manifest_path).unwrap();

    assert_eq!(sample_path, root.path().join("frame.shadowrawi.u16le"));
    assert_eq!(descriptor.width, 4);
    assert_eq!(descriptor.height, 4);
    assert_eq!(descriptor.cfa, "RGGB");
    assert_eq!(descriptor.black_levels, [64; 4]);
    assert_eq!(descriptor.white_levels, [16_383; 4]);
    assert_eq!(descriptor.sample_bytes, 32);
    assert_eq!(descriptor.decoded_samples_sha256, SAMPLE_SHA256);
    assert_eq!(descriptor.decoder_provider_id, "shadow.test.raw");
    assert_eq!(descriptor.decoder_provider_version, "v1");
    let mut bytes = Vec::new();
    std::io::Read::read_to_end(&mut sample, &mut bytes).unwrap();
    assert_eq!(bytes, [0_u8, 1, 2, 3].repeat(8));

    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt as _;

        assert_eq!(
            fs::metadata(&manifest_path).unwrap().permissions().mode() & 0o777,
            0o600
        );
        assert_eq!(
            fs::metadata(&sample_path).unwrap().permissions().mode() & 0o777,
            0o600
        );
    }
}

#[test]
fn rejects_additive_or_duplicate_manifest_fields() {
    let root = FixtureRoot::new();
    let manifest_path = write_fixture(root.path(), " unexpected=value");
    assert!(open_staged_frame(&manifest_path).is_err());

    let mut manifest = fs::OpenOptions::new()
        .append(true)
        .open(&manifest_path)
        .unwrap();
    writeln!(manifest, "width=4").unwrap();
    assert!(open_staged_frame(&manifest_path).is_err());
}
