//! Fallible conversion of the host wall clock into catalog milliseconds.

use std::time::{SystemTime, UNIX_EPOCH};

use anyhow::{Context, Result as AnyResult};

pub(crate) fn current_time_ms() -> AnyResult<i64> {
    let milliseconds = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .context("system time is before the Unix epoch")?
        .as_millis();
    i64::try_from(milliseconds).context("current time does not fit in signed milliseconds")
}
