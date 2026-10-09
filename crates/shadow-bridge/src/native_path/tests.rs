//! Exact host path payloads, independent of filesystem filename admission.

#[cfg(unix)]
#[test]
fn unix_path_payload_preserves_non_utf8_bytes_without_a_display_conversion() {
    use std::{ffi::OsStr, os::unix::ffi::OsStrExt, path::Path};

    let bytes = b"synthetic-\xff/source.png";
    let path = Path::new(OsStr::from_bytes(bytes));
    assert!(path.to_str().is_none());
    let payload = super::bridge_path(path).expect("Unix paths have a byte transport");
    assert_eq!(payload, bytes);
    assert_eq!(
        payload.as_ptr(),
        bytes.as_ptr(),
        "the bridge borrows the native path"
    );
}
