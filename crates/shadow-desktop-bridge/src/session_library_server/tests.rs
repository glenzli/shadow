#![cfg(windows)]

use std::os::windows::ffi::OsStrExt as _;

use super::start_request;
use crate::ffi::{FfiLibraryServerConfig, FfiNativePath, FfiNativePathPlatform};

#[test]
fn shared_unc_root_uses_its_native_utf16_payload() {
    let units: Vec<u16> = r"\\?\UNC\nas\摄影\emoji😀".encode_utf16().collect();
    let request = start_request(&FfiLibraryServerConfig {
        bind_address: "127.0.0.1:37641".to_owned(),
        authorization: "01234567890123456789012345678901".to_owned(),
        display_name: "Windows Server".to_owned(),
        share_roots: vec![FfiNativePath {
            platform: FfiNativePathPlatform::Windows,
            unix_bytes: Vec::new(),
            windows_units: units.clone(),
            display_path: r"C:\not-the-shared-root".to_owned(),
        }],
        serves_originals: true,
    })
    .expect("build server request");

    let actual: Vec<u16> = request.share_roots[0].as_os_str().encode_wide().collect();
    assert_eq!(actual, units);
}
