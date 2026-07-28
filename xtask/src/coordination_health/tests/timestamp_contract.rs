use super::super::time::parse_rfc3339_seconds;

#[test]
fn rfc3339_parser_normalizes_offsets_and_leap_days() {
    assert_eq!(parse_rfc3339_seconds("1970-01-01T00:00:00Z"), Ok(0));
    assert_eq!(parse_rfc3339_seconds("1970-01-01T08:00:00+08:00"), Ok(0));
    assert!(parse_rfc3339_seconds("2025-02-29T00:00:00Z").is_err());
    assert!(parse_rfc3339_seconds("2024-02-29T00:00:00Z").is_ok());
}
