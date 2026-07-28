//! Test-only wire fixtures shared by metadata and decoder descriptors.

use super::{
    decoder_snapshot::{DECODER_SNAPSHOT_BASE_FIELD_COUNT, DECODER_SNAPSHOT_PREVIEW_FIELD_COUNT},
    metadata::{METADATA_SNAPSHOT_FIELD_COUNT, RAW_METADATA_SNAPSHOT_FIELD_COUNT},
    route_identity::{DECODER_SNAPSHOT_PROTOCOL, METADATA_SNAPSHOT_PROTOCOL},
};

pub(super) fn metadata_snapshot_protocol(nonce: &str) -> Vec<u8> {
    const ZERO: &str = "0000000000000000";
    const ONE: &str = "0000000000000001";
    let mut fields = vec![
        METADATA_SNAPSHOT_PROTOCOL,
        "metadata-snapshot",
        nonce,
        "736861646f772d726f75746572", // shadow-router
        "7631",                       // v1
        "4e696b6f6e",                 // Nikon
        "5a2039",                     // Z 9
        "4e696b6f6e",                 // Nikon
        "5a2039",                     // Z 9
        "-",                          // absent DNG version
    ];
    fields.extend([
        ONE,                // raw count
        "0000000000002040", // raw width
        "0000000000001580", // raw height
        "0000000000002040", // image width
        "0000000000001580", // image height
        ZERO,
        ZERO,
        ZERO,
        ZERO,
        ONE,        // orientation
        "52474742", // RGGB
        "0000000000000003",
        "000000000000000e",
        "0000000000000200",
        "0000000000003fff",
        "3ff0000000000000",
        "3ff0000000000000",
        "3ff0000000000000",
        "3ff0000000000000",
        ZERO,
        "4059000000000000", // ISO 100
        "3f80624dd2f1a9fc", // 1/125s
        "4016666666666666", // f/5.6
        "4038000000000000", // 24mm
        "0000000065c91400",
        "4e494b4f4e",                         // NIKON
        "4e494b4b4f52205a2032342d3132306d6d", // NIKKOR Z 24-120mm
        "4042000000000000",                   // 36mm equivalent
    ]);
    assert_eq!(fields.len(), METADATA_SNAPSHOT_FIELD_COUNT);
    fields.join(" ").into_bytes()
}

pub(super) fn decoder_snapshot_protocol(nonce: &str) -> Vec<u8> {
    const ZERO: &str = "0000000000000000";
    const ONE: &str = "0000000000000001";
    let metadata_fields = String::from_utf8(metadata_snapshot_protocol(nonce))
        .expect("metadata fixture is UTF-8")
        .split_whitespace()
        .skip(5)
        .map(ToOwned::to_owned)
        .collect::<Vec<_>>();
    assert_eq!(metadata_fields.len(), RAW_METADATA_SNAPSHOT_FIELD_COUNT);

    let mut fields = vec![
        DECODER_SNAPSHOT_PROTOCOL.to_owned(),
        "decoder-snapshot".to_owned(),
        nonce.to_owned(),
        "736861646f772d68656c706572".to_owned(), // shadow-helper
        "76332d70726976617465".to_owned(),       // v3-private
        ONE.to_owned(),
        ZERO.to_owned(),
        ONE.to_owned(),
    ];
    fields.extend(metadata_fields);
    fields.extend([
        ONE.to_owned(),                // metadata
        ONE.to_owned(),                // embedded previews
        ONE.to_owned(),                // raw frame
        ONE.to_owned(),                // reference RGB
        ZERO.to_owned(),               // opcode list 1
        ZERO.to_owned(),               // opcode list 2
        ZERO.to_owned(),               // opcode list 3
        ONE.to_owned(),                // development schema
        ONE.to_owned(),                // development available
        ONE.to_owned(),                // development raw frame
        ZERO.to_owned(),               // opcode receipt
        "0000000000000007".to_owned(), // intents
        "0000000000000006".to_owned(), // qualities
        ONE.to_owned(),                // opcode policies
        ONE.to_owned(),                // denoise intents
        "0000000000000003".to_owned(), // highlight intents
        "0000000000000002".to_owned(), // preview count
        // JPEG preview descriptor.
        ZERO.to_owned(),
        ONE.to_owned(),
        "0000000000000fa0".to_owned(),
        "0000000000000bb8".to_owned(),
        "0000000000000008".to_owned(),
        "0000000000000003".to_owned(),
        "0000000000123456".to_owned(),
        ONE.to_owned(),
        // A second, unavailable/unknown descriptor must still round-trip
        // as a descriptor; no preview bytes cross the helper boundary.
        "0000000000000005".to_owned(),
        ZERO.to_owned(),
        ZERO.to_owned(),
        ZERO.to_owned(),
        ZERO.to_owned(),
        ZERO.to_owned(),
        ZERO.to_owned(),
        ZERO.to_owned(),
    ]);
    assert_eq!(
        fields.len(),
        DECODER_SNAPSHOT_BASE_FIELD_COUNT + 2 * DECODER_SNAPSHOT_PREVIEW_FIELD_COUNT
    );
    fields.join(" ").into_bytes()
}
