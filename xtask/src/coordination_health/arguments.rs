use super::invalid_argument;
use std::{ffi::OsString, io, path::PathBuf, time::Duration};

pub(super) const DEFAULT_STALE_AFTER: Duration = Duration::from_mins(30);

#[derive(Debug, Default)]
pub(super) struct HealthArguments {
    pub(super) root: Option<PathBuf>,
    pub(super) stale_after: Duration,
    pub(super) fail_on_stale: bool,
    pub(super) gate: GateMode,
    pub(super) strict: bool,
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub(super) enum GateMode {
    #[default]
    None,
    ScopedCommit,
    BulkStage,
}

impl HealthArguments {
    pub(super) fn parse(arguments: impl IntoIterator<Item = OsString>) -> io::Result<Self> {
        let mut arguments = arguments.into_iter();
        let mut parsed = Self {
            stale_after: DEFAULT_STALE_AFTER,
            ..Self::default()
        };

        while let Some(argument) = arguments.next() {
            match argument.to_string_lossy().as_ref() {
                "--root" => {
                    let value = arguments.next().ok_or_else(|| {
                        invalid_argument("coordination-health --root requires a path")
                    })?;
                    parsed.root = Some(PathBuf::from(value));
                }
                "--stale-after-minutes" => {
                    let value = arguments.next().ok_or_else(|| {
                        invalid_argument(
                            "coordination-health --stale-after-minutes requires a whole number",
                        )
                    })?;
                    let minutes = value.to_string_lossy().parse::<u64>().map_err(|_| {
                        invalid_argument("stale-after-minutes must be a whole number")
                    })?;
                    if minutes == 0 {
                        return Err(invalid_argument("stale-after-minutes must be positive"));
                    }
                    parsed.stale_after = Duration::from_secs(minutes.saturating_mul(60));
                }
                "--fail-on-stale" => parsed.fail_on_stale = true,
                "--commit-gate" => parsed.set_gate(GateMode::ScopedCommit)?,
                "--bulk-stage-gate" => parsed.set_gate(GateMode::BulkStage)?,
                "--strict" => parsed.strict = true,
                "--help" | "-h" => {
                    return Err(invalid_argument(
                        "usage: cargo xtask coordination-health [--root PATH] [--stale-after-minutes N] [--fail-on-stale] [--strict] [--commit-gate] [--bulk-stage-gate]",
                    ));
                }
                other => {
                    return Err(invalid_argument(&format!(
                        "unknown coordination-health argument: {other}",
                    )));
                }
            }
        }

        Ok(parsed)
    }

    fn set_gate(&mut self, requested: GateMode) -> io::Result<()> {
        if self.gate != GateMode::None {
            return Err(invalid_argument(
                "choose either --commit-gate or --bulk-stage-gate",
            ));
        }
        self.gate = requested;
        Ok(())
    }
}
