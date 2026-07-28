use super::capture_day;

#[test]
fn civil_day_handles_epoch_and_negative_subday_values() {
    assert_eq!(capture_day(0), "");
    assert_eq!(capture_day(1), "1970-01-01");
    assert_eq!(capture_day(-1), "1969-12-31");
    assert_eq!(capture_day(1_700_000_000), "2023-11-14");
}
