use super::{AuthorizationToken, constant_time_equal};

#[test]
fn token_is_redacted_and_requires_real_entropy_budget() {
    assert!(AuthorizationToken::parse("short").is_err());
    let token = AuthorizationToken::parse("01234567890123456789012345678901").expect("valid token");
    assert_eq!(format!("{token:?}"), "AuthorizationToken(\"<redacted>\")");
}

#[test]
fn token_comparison_rejects_different_lengths_and_values() {
    assert!(constant_time_equal(b"same", b"same"));
    assert!(!constant_time_equal(b"same", b"diff"));
    assert!(!constant_time_equal(b"same", b"same-longer"));
}
