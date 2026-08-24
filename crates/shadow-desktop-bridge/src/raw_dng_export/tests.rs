use std::{collections::BTreeMap, fs, io::Read as _};

use sha2::{Digest as _, Sha256};
use tempfile::tempdir;

use super::{PRIVATE_MAGIC, RawDngPublishOutcome, TYPE_BYTE, TYPE_LONG, encode_parts};
use crate::{digest_hex::encode_hex, isolated_proxy::IsolatedRawFrameDescriptor};

fn descriptor(samples: &[u8]) -> IsolatedRawFrameDescriptor {
    IsolatedRawFrameDescriptor {
        width: 4,
        height: 4,
        cfa: "RGGB".to_owned(),
        black_levels: [64, 65, 66, 67],
        white_levels: [16_383, 16_200, 16_100, 16_000],
        linear_response_limits: [15_100, 15_101, 15_102, 15_103],
        has_linear_response_limits: true,
        orientation: 0,
        bits_per_sample: 14,
        as_shot_neutral: [1.8, 1.0, 1.2, 1.5],
        camera_to_xyz_d50: Some([1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0]),
        xyz_to_camera_d65: Some([1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0]),
        camera_to_linear_srgb_d65: None,
        pending_dng_opcode_bytes: [11, 22, 33],
        sample_bytes: u64::try_from(samples.len()).unwrap(),
        decoded_samples_sha256: encode_hex(Sha256::digest(samples).as_slice()),
        decoder_provider_id: "shadow.test.raw".to_owned(),
        decoder_provider_version: "v1".to_owned(),
    }
}

fn parse_entries(bytes: &[u8]) -> BTreeMap<u16, (u16, u32, Vec<u8>)> {
    assert_eq!(&bytes[..8], b"II*\0\x08\0\0\0");
    let count = usize::from(u16::from_le_bytes(bytes[8..10].try_into().unwrap()));
    let mut entries = BTreeMap::new();
    for index in 0..count {
        let offset = 10 + index * 12;
        let tag = u16::from_le_bytes(bytes[offset..offset + 2].try_into().unwrap());
        let field_type = u16::from_le_bytes(bytes[offset + 2..offset + 4].try_into().unwrap());
        let value_count = u32::from_le_bytes(bytes[offset + 4..offset + 8].try_into().unwrap());
        let type_size = match field_type {
            1 | 2 => 1,
            3 => 2,
            4 => 4,
            5 | 10 => 8,
            _ => panic!("unexpected TIFF field type"),
        };
        let byte_count = usize::try_from(value_count).unwrap() * type_size;
        let data = if byte_count <= 4 {
            bytes[offset + 8..offset + 8 + byte_count].to_vec()
        } else {
            let value_offset = usize::try_from(u32::from_le_bytes(
                bytes[offset + 8..offset + 12].try_into().unwrap(),
            ))
            .unwrap();
            bytes[value_offset..value_offset + byte_count].to_vec()
        };
        entries.insert(tag, (field_type, value_count, data));
    }
    entries
}

#[test]
fn writes_byte_exact_cfa_and_exact_private_descriptor() {
    let root = tempdir().unwrap();
    let samples = [0_u8, 1, 2, 3].repeat(8);
    let sample_path = root.path().join("samples.u16le");
    fs::write(&sample_path, &samples).unwrap();
    let mut sample = fs::File::open(&sample_path).unwrap();
    let output = root.path().join("normalized.dng");

    let pending = encode_parts(&descriptor(&samples), &mut sample, &output).unwrap();
    let receipt = match pending.publish().unwrap() {
        RawDngPublishOutcome::Published(receipt) => receipt,
        RawDngPublishOutcome::OutputConflict => panic!("unexpected conflict"),
    };
    let encoded = fs::read(&output).unwrap();
    let entries = parse_entries(&encoded);
    let strip_offset =
        usize::try_from(u32::from_le_bytes(entries[&273].2[..4].try_into().unwrap())).unwrap();
    let strip_bytes =
        usize::try_from(u32::from_le_bytes(entries[&279].2[..4].try_into().unwrap())).unwrap();
    assert_eq!(&encoded[strip_offset..strip_offset + strip_bytes], samples);
    assert_eq!(receipt.byte_len, u64::try_from(encoded.len()).unwrap());
    assert_eq!(receipt.content_digest, Sha256::digest(&encoded).as_slice());
    assert_eq!(entries[&50_717].0, TYPE_LONG);
    assert_eq!(
        u32::from_le_bytes(entries[&50_717].2[..4].try_into().unwrap()),
        16_000
    );
    assert_eq!(entries[&50_740].0, TYPE_BYTE);
    assert!(entries[&50_740].2.starts_with(PRIVATE_MAGIC));
    let private: serde_json::Value =
        serde_json::from_slice(&entries[&50_740].2[PRIVATE_MAGIC.len()..]).unwrap();
    assert_eq!(private["schema"], "shadow.raw-dng-export.v1");
    assert_eq!(private["normalized_mosaic"]["white"][0], 16_383);
    assert_eq!(
        private["normalized_mosaic"]["pending_dng_opcode_bytes"][2],
        33
    );
    assert_eq!(private["recipe_applied"], false);
    assert_eq!(private["pending_opcodes_applied"], false);
}

#[test]
fn publication_never_overwrites_an_existing_destination() {
    let root = tempdir().unwrap();
    let samples = [0_u8, 1, 2, 3].repeat(8);
    let sample_path = root.path().join("samples.u16le");
    fs::write(&sample_path, &samples).unwrap();
    let output = root.path().join("existing.dng");
    fs::write(&output, b"keep-me").unwrap();
    let mut sample = fs::File::open(&sample_path).unwrap();

    let outcome = encode_parts(&descriptor(&samples), &mut sample, &output)
        .unwrap()
        .publish()
        .unwrap();

    assert!(matches!(outcome, RawDngPublishOutcome::OutputConflict));
    let mut existing = Vec::new();
    fs::File::open(output)
        .unwrap()
        .read_to_end(&mut existing)
        .unwrap();
    assert_eq!(existing, b"keep-me");
}
