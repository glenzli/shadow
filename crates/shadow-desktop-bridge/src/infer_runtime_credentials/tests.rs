use super::*;

#[test]
fn qt_nested_application_root_uses_shared_credentials() {
    assert_eq!(
        default_credential_file(Path::new(
            "/Users/test/Library/Application Support/Shadow/Shadow"
        )),
        Path::new(
            "/Users/test/Library/Application Support/Shadow/credentials/infer-runtime-shadow.token"
        )
    );
}

#[test]
fn isolated_roots_do_not_fall_back_to_user_credentials() {
    for root in [
        "/private/tmp/shadow-test",
        "/Users/test/Library/Application Support/Shadow",
        "/",
    ] {
        assert_eq!(
            default_credential_file(Path::new(root)),
            Path::new(root).join("credentials/infer-runtime-shadow.token")
        );
    }
}
