use std::fs;

use super::token_from_file;

#[test]
fn sharing_token_is_loaded_without_its_trailing_newline() {
    let path = std::env::temp_dir().join(format!("shadow-library-token-{}", uuid::Uuid::now_v7()));
    fs::write(&path, "01234567890123456789012345678901\n").expect("write token");
    token_from_file(path.to_str().expect("token path")).expect("load token");
    fs::remove_file(path).expect("remove token");
}
