//! Chinese lunar-calendar projection for indexed Library date filtering.
//!
//! The Catalog stores this compact projection beside the canonical Gregorian
//! capture day. Conversion happens only when metadata enters or changes; hot
//! Library queries compare integers and never walk the photo set to convert
//! dates on demand.

use chinese_lunisolar_calendar::{LunisolarDate, SolarDate};

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub(crate) struct ChineseLunarDate {
    pub month: u8,
    pub day: u8,
    pub is_leap_month: bool,
}

/// Projects one canonical Gregorian `YYYY-MM-DD` day into the supported
/// Chinese lunisolar range. Missing, malformed, and out-of-range dates stay
/// unindexed instead of making ordinary Library metadata invalid.
pub(crate) fn chinese_lunar_date(capture_day: &str) -> Option<ChineseLunarDate> {
    let bytes = capture_day.as_bytes();
    if bytes.len() != 10 || bytes[4] != b'-' || bytes[7] != b'-' {
        return None;
    }
    let year = capture_day[..4].parse().ok()?;
    let month = capture_day[5..7].parse().ok()?;
    let day = capture_day[8..].parse().ok()?;
    let lunar = LunisolarDate::from_solar_date(SolarDate::from_ymd(year, month, day).ok()?).ok()?;
    let lunar_month = lunar.to_lunar_month();
    Some(ChineseLunarDate {
        month: lunar_month.to_u8(),
        day: lunar.to_lunar_day().to_u8(),
        is_leap_month: lunar_month.is_leap_month(),
    })
}

#[cfg(test)]
mod tests;
