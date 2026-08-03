use super::{ChineseLunarDate, chinese_lunar_date};

#[test]
fn projects_spring_festival_and_mid_autumn_days() {
    assert_eq!(
        chinese_lunar_date("2024-02-10"),
        Some(ChineseLunarDate {
            month: 1,
            day: 1,
            is_leap_month: false,
        })
    );
    assert_eq!(
        chinese_lunar_date("2023-09-29"),
        Some(ChineseLunarDate {
            month: 8,
            day: 15,
            is_leap_month: false,
        })
    );
}

#[test]
fn preserves_leap_month_identity() {
    assert_eq!(
        chinese_lunar_date("2023-03-22"),
        Some(ChineseLunarDate {
            month: 2,
            day: 1,
            is_leap_month: true,
        })
    );
}

#[test]
fn leaves_malformed_or_unsupported_days_unindexed() {
    assert_eq!(chinese_lunar_date(""), None);
    assert_eq!(chinese_lunar_date("2024-02-30"), None);
    assert_eq!(chinese_lunar_date("2200-01-01"), None);
}
