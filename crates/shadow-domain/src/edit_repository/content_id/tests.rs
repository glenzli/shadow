use std::str::FromStr;

use super::*;

#[test]
fn repository_ids_round_trip_as_fixed_width_hex() {
    let object = EditObjectId::from_bytes([0x5a; 32]);
    let commit = EditCommitId::from_bytes([0xa5; 32]);

    for encoded in [object.to_string(), commit.to_string()] {
        assert_eq!(encoded.len(), 64);
        assert_eq!(encoded, encoded.to_ascii_lowercase());
    }
    assert_eq!(EditObjectId::from_str(&object.to_string()).unwrap(), object);
    assert_eq!(EditCommitId::from_str(&commit.to_string()).unwrap(), commit);
    assert_eq!(
        serde_json::from_str::<EditObjectId>(&serde_json::to_string(&object).unwrap()).unwrap(),
        object
    );
}

#[test]
fn repository_ids_reject_malformed_hex() {
    for invalid in ["", "00", &"g".repeat(64), &"0".repeat(63)] {
        assert!(matches!(
            EditObjectId::from_str(invalid),
            Err(EditRepositoryError::InvalidDigest(value)) if value == invalid
        ));
    }
}
