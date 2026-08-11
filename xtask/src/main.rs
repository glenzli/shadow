mod coordination_health;
mod daily_use_smoke;
mod desktop_i18n;
mod desktop_workflow;
mod doctor;
mod format;
mod library_scale_smoke;
mod local_workspace_guard;
mod raw_smoke;
mod test_layout;
mod workspace_build;

use std::{env, io};

fn main() -> io::Result<()> {
    let command = env::args().nth(1).unwrap_or_else(|| "help".to_owned());
    match command.as_str() {
        "check" => {
            test_layout::run(std::iter::empty())?;
            workspace_build::run("cargo", &["fmt", "--check"])?;
            workspace_build::run(
                "cargo",
                &[
                    "clippy",
                    "--workspace",
                    "--all-targets",
                    "--",
                    "-D",
                    "warnings",
                ],
            )
        }
        "test" => workspace_build::run("cargo", &["test", "--workspace"]),
        "format" => format::run(env::args_os().skip(2)),
        "native-configure" => workspace_build::configure_preset("native-dev").map(|_| ()),
        "native-build" => workspace_build::build_preset("native-dev"),
        "desktop-build" => {
            desktop_i18n::run()?;
            let build_directory = workspace_build::configure_preset("desktop-dev")?;
            workspace_build::build_directory_contents(&build_directory)
        }
        "desktop-check" => {
            desktop_i18n::run()?;
            let build_directory = workspace_build::configure_preset("desktop-dev")?;
            workspace_build::build_directory_contents(&build_directory)?;
            workspace_build::run_ctest(&build_directory)
        }
        "desktop-release" => {
            desktop_i18n::run()?;
            let build_directory = workspace_build::configure_preset("desktop-release")?;
            workspace_build::build_directory_contents(&build_directory)
        }
        "desktop-build-promote" => desktop_workflow::build_and_promote(env::args_os().skip(2)),
        "desktop-promote-debug" => desktop_workflow::promote(env::args_os().skip(2)),
        "desktop-run-debug" => desktop_workflow::launch(env::args_os().skip(2)),
        "desktop-smoke" | "daily-use-smoke" => daily_use_smoke::run(env::args_os().nth(2)),
        "desktop-i18n-check" => desktop_i18n::run(),
        "native-check" => {
            let build_directory = workspace_build::configure_preset("native-dev")?;
            workspace_build::build_directory_contents(&build_directory)?;
            workspace_build::run_ctest(&build_directory)
        }
        "raw-smoke" => raw_smoke::run(env::args_os().nth(2)),
        "library-scale-smoke" => library_scale_smoke::run(env::args_os().skip(2)),
        "coordination-health" => coordination_health::run(env::args_os().skip(2)),
        "local-workspace-guard" => local_workspace_guard::run(env::args_os().skip(2)),
        "test-layout" => test_layout::run(env::args_os().skip(2)),
        "doctor" => {
            doctor::run();
            Ok(())
        }
        _ => {
            println!(
                "cargo xtask <check|test|format [--check] [--all] [native-source ...]|test-layout [--root PATH] [--verbose] [--print-observed]|native-configure|native-build|native-check|desktop-i18n-check|desktop-build|desktop-check|desktop-release|desktop-build-promote [validation-label|--check]|desktop-promote-debug APP LABEL|desktop-run-debug [--check|--foreground]|desktop-smoke [fixture-directory]|raw-smoke [fixture-directory]|daily-use-smoke [fixture-directory]|library-scale-smoke [--photos N] [--page-size N]|coordination-health [--root PATH] [--stale-after-minutes N] [--fail-on-stale] [--strict] [--commit-gate] [--bulk-stage-gate]|local-workspace-guard [--root PATH]|doctor>"
            );
            Ok(())
        }
    }
}
