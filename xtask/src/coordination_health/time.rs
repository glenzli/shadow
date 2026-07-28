use std::io;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

pub(super) fn now_unix_seconds() -> io::Result<i64> {
    let duration = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_err(|error| io::Error::other(format!("system clock is before Unix epoch: {error}")))?;
    i64::try_from(duration.as_secs())
        .map_err(|_| io::Error::other("system time exceeds i64 Unix seconds"))
}

pub(super) fn duration_seconds(duration: Duration) -> i64 {
    i64::try_from(duration.as_secs()).unwrap_or(i64::MAX)
}

pub(super) fn format_duration(seconds: i64) -> String {
    if seconds >= 3600 {
        format!("{}h {}m", seconds / 3600, (seconds % 3600) / 60)
    } else if seconds >= 60 {
        format!("{}m", seconds / 60)
    } else {
        format!("{seconds}s")
    }
}

pub(super) fn parse_rfc3339_seconds(value: &str) -> Result<i64, String> {
    let bytes = value.as_bytes();
    if bytes.len() < 20
        || bytes.get(4) != Some(&b'-')
        || bytes.get(7) != Some(&b'-')
        || bytes.get(10) != Some(&b'T')
        || bytes.get(13) != Some(&b':')
        || bytes.get(16) != Some(&b':')
    {
        return Err("expected RFC3339 date-time".to_owned());
    }
    let year = parse_component(bytes, 0, 4)?;
    let month = parse_component(bytes, 5, 2)?;
    let day = parse_component(bytes, 8, 2)?;
    let hour = parse_component(bytes, 11, 2)?;
    let minute = parse_component(bytes, 14, 2)?;
    let second = parse_component(bytes, 17, 2)?;
    validate_date_time(year, month, day, hour, minute, second)?;

    let mut timezone_start = 19;
    if bytes.get(timezone_start) == Some(&b'.') {
        timezone_start += 1;
        while bytes.get(timezone_start).is_some_and(u8::is_ascii_digit) {
            timezone_start += 1;
        }
    }
    let offset_seconds = parse_timezone_offset(&bytes[timezone_start..])?;
    let days = days_from_civil(year, month, day);
    let local_seconds = days
        .checked_mul(86_400)
        .and_then(|seconds| seconds.checked_add(i64::from(hour) * 3_600))
        .and_then(|seconds| seconds.checked_add(i64::from(minute) * 60))
        .and_then(|seconds| seconds.checked_add(i64::from(second)))
        .ok_or_else(|| "timestamp exceeds i64 Unix seconds".to_owned())?;
    local_seconds
        .checked_sub(i64::from(offset_seconds))
        .ok_or_else(|| "timestamp offset exceeds i64 Unix seconds".to_owned())
}

fn parse_component(bytes: &[u8], start: usize, length: usize) -> Result<i32, String> {
    let component = bytes
        .get(start..start + length)
        .ok_or_else(|| "timestamp is truncated".to_owned())?;
    if !component.iter().all(u8::is_ascii_digit) {
        return Err("timestamp contains a non-numeric component".to_owned());
    }
    component.iter().try_fold(0_i32, |value, digit| {
        value
            .checked_mul(10)
            .and_then(|value| value.checked_add(i32::from(*digit - b'0')))
            .ok_or_else(|| "timestamp component overflows".to_owned())
    })
}

fn validate_date_time(
    year: i32,
    month: i32,
    day: i32,
    hour: i32,
    minute: i32,
    second: i32,
) -> Result<(), String> {
    if !(1..=12).contains(&month) {
        return Err("month is outside 1..=12".to_owned());
    }
    let month = u32::try_from(month).map_err(|_| "invalid month".to_owned())?;
    let maximum_day = match month {
        1 | 3 | 5 | 7 | 8 | 10 | 12 => 31,
        4 | 6 | 9 | 11 => 30,
        2 if is_leap_year(year) => 29,
        2 => 28,
        _ => unreachable!("month was range checked"),
    };
    if !(1..=maximum_day).contains(&day) {
        return Err("day is outside the selected month".to_owned());
    }
    if !(0..=23).contains(&hour) || !(0..=59).contains(&minute) || !(0..=59).contains(&second) {
        return Err("time is outside RFC3339 whole-second bounds".to_owned());
    }
    Ok(())
}

fn parse_timezone_offset(bytes: &[u8]) -> Result<i32, String> {
    match bytes {
        [b'Z'] => Ok(0),
        [
            sign @ (b'+' | b'-'),
            hour_a,
            hour_b,
            b':',
            minute_a,
            minute_b,
        ] => {
            let hour = parse_component(&[*hour_a, *hour_b], 0, 2)?;
            let minute = parse_component(&[*minute_a, *minute_b], 0, 2)?;
            if hour > 23 || minute > 59 {
                return Err("timezone offset is outside RFC3339 bounds".to_owned());
            }
            let seconds = hour * 3_600 + minute * 60;
            Ok(if *sign == b'+' { seconds } else { -seconds })
        }
        _ => Err("timezone must be Z or ±HH:MM".to_owned()),
    }
}

fn is_leap_year(year: i32) -> bool {
    year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)
}

fn days_from_civil(year: i32, month: i32, day: i32) -> i64 {
    let year = i64::from(year) - i64::from(month <= 2);
    let era = if year >= 0 { year } else { year - 399 } / 400;
    let year_of_era = year - era * 400;
    let adjusted_month = i64::from(month) + if month > 2 { -3 } else { 9 };
    let day_of_year = (153 * adjusted_month + 2) / 5 + i64::from(day) - 1;
    let day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    era * 146_097 + day_of_era - 719_468
}
