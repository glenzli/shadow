use std::{
    env,
    ffi::OsString,
    io,
    path::{Path, PathBuf},
    process::Command,
};

use super::{
    layout::{AppBundlePaths, CITY_INDEX_NAME, WorkflowPaths},
    process, promotion, run,
};

#[derive(Debug, Eq, PartialEq)]
enum BuildRequest {
    Help,
    Check,
    Build {
        validation_label: Option<String>,
        verify_startup: bool,
    },
}

pub(super) fn run(arguments: impl IntoIterator<Item = OsString>) -> io::Result<()> {
    let request = parse(arguments)?;
    if request == BuildRequest::Help {
        print_usage();
        return Ok(());
    }
    #[cfg(not(target_os = "macos"))]
    {
        let _ = request;
        return Err(io::Error::new(
            io::ErrorKind::Unsupported,
            "desktop build-and-promote currently has only a macOS backend; use the Windows CMake presets for compilation",
        ));
    }
    #[cfg(target_os = "macos")]
    run_macos(request)
}

#[cfg(target_os = "macos")]
fn run_macos(request: BuildRequest) -> io::Result<()> {
    let paths = WorkflowPaths::resolve()?;
    let city_index = resolve_city_index(&paths);
    let provider_directory = resolve_provider_directory(&paths);
    let provider = provider_directory.join(WorkflowPaths::provider_name());
    require_file(&city_index, "no prepared GeoNames city index")?;
    if !process::is_executable(&provider) {
        return Err(io::Error::new(
            io::ErrorKind::NotFound,
            format!(
                "no frozen RawNIND provider; set SHADOW_RAWNIND_FOUNDATION_PROVIDER_DIR (expected {})",
                provider.display()
            ),
        ));
    }
    if request == BuildRequest::Check {
        println!(
            "candidate build directory: {}",
            paths.build_directory.display()
        );
        println!("GeoNames city index input: {}", city_index.display());
        println!("RawNIND provider input: {}", provider_directory.display());
        return Ok(());
    }
    let BuildRequest::Build {
        validation_label,
        verify_startup,
    } = request
    else {
        unreachable!("help and check returned above");
    };
    let validation_label = validation_label.map_or_else(
        || {
            short_revision(&paths.repository_root).map(|revision| {
                let mode = if verify_startup { "verified" } else { "fast" };
                format!("canonical-debug-{mode}-{revision}")
            })
        },
        Ok,
    )?;
    crate::local_workspace_guard::run([
        OsString::from("--root"),
        paths.repository_root.clone().into_os_string(),
    ])?;
    crate::desktop_i18n::run()?;
    process::run(
        Command::new("cmake")
            .current_dir(&paths.repository_root)
            .arg("--preset")
            .arg("desktop-dev")
            .arg("-B")
            .arg(&paths.build_directory)
            .arg(format!(
                "-DSHADOW_GEONAMES_CITY_INDEX_PATH={}",
                city_index.display()
            ))
            .arg(format!(
                "-DSHADOW_RAWNIND_FOUNDATION_PROVIDER_DIR={}",
                provider_directory.display()
            )),
        "configure canonical debug desktop",
    )?;
    process::run(
        Command::new("cmake")
            .arg("--build")
            .arg(&paths.build_directory)
            .args([
                "--target",
                "shadow-desktop",
                "shadow-server-manager",
                "--parallel",
                "6",
            ]),
        "build canonical debug desktop",
    )?;
    if verify_startup {
        process::run(
            Command::new("ctest")
                .arg("--test-dir")
                .arg(&paths.build_directory)
                .args([
                    "--output-on-failure",
                    "-R",
                    "^(shadow-desktop-qml-startup|shadow-server-manager-qml-startup)$",
                ]),
            "run canonical debug startup smoke",
        )?;
    } else {
        println!(
            "fast promotion: startup smoke skipped; use `cargo xtask desktop-build-promote --verify` when it is needed"
        );
    }
    let candidate = paths.build_directory.join("apps/desktop/Shadow.app");
    promotion::promote_candidate(&candidate, &validation_label)?;
    run::check()
}

fn parse(arguments: impl IntoIterator<Item = OsString>) -> io::Result<BuildRequest> {
    let values = arguments.into_iter().collect::<Vec<_>>();
    match values.as_slice() {
        [] => Ok(BuildRequest::Build {
            validation_label: None,
            verify_startup: false,
        }),
        [argument] if argument == "--help" || argument == "-h" => Ok(BuildRequest::Help),
        [argument] if argument == "--check" => Ok(BuildRequest::Check),
        [verify] if verify == "--verify" => Ok(BuildRequest::Build {
            validation_label: None,
            verify_startup: true,
        }),
        [verify, check] if verify == "--verify" && check == "--check" => Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "--check cannot be combined with --verify",
        )),
        [check, verify] if check == "--check" && verify == "--verify" => Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "--check cannot be combined with --verify",
        )),
        [verify, label] if verify == "--verify" => Ok(BuildRequest::Build {
            validation_label: Some(label.to_string_lossy().into_owned()),
            verify_startup: true,
        }),
        [label] => Ok(BuildRequest::Build {
            validation_label: Some(label.to_string_lossy().into_owned()),
            verify_startup: false,
        }),
        [label, verify] if verify == "--verify" => Ok(BuildRequest::Build {
            validation_label: Some(label.to_string_lossy().into_owned()),
            verify_startup: true,
        }),
        _ => Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "usage: cargo xtask desktop-build-promote [--verify] [validation-label]\n       cargo xtask desktop-build-promote --check",
        )),
    }
}

#[cfg(target_os = "macos")]
fn resolve_city_index(paths: &WorkflowPaths) -> PathBuf {
    let configured = env::var_os("SHADOW_GEONAMES_CITY_INDEX_PATH").map_or_else(
        || paths.asset_root.join("geonames").join(CITY_INDEX_NAME),
        PathBuf::from,
    );
    if configured.is_file() {
        configured
    } else {
        let cached = AppBundlePaths::macos(paths.current_app.clone()).geonames_index;
        if cached.is_file() { cached } else { configured }
    }
}

#[cfg(target_os = "macos")]
fn resolve_provider_directory(paths: &WorkflowPaths) -> PathBuf {
    let configured = env::var_os("SHADOW_RAWNIND_FOUNDATION_PROVIDER_DIR")
        .map_or_else(|| paths.asset_root.join("rawnind"), PathBuf::from);
    if process::is_executable(&configured.join(WorkflowPaths::provider_name())) {
        configured
    } else {
        let cached = paths.current_app.join("Contents/Helpers/RawNIND");
        if process::is_executable(&cached.join(WorkflowPaths::provider_name())) {
            cached
        } else {
            configured
        }
    }
}

#[cfg(target_os = "macos")]
fn short_revision(repository_root: &Path) -> io::Result<String> {
    process::capture(
        Command::new("git")
            .current_dir(repository_root)
            .args(["rev-parse", "--short", "HEAD"]),
        "resolve source revision",
    )
}

#[cfg(target_os = "macos")]
fn require_file(path: &Path, message: &str) -> io::Result<()> {
    if path.is_file() {
        Ok(())
    } else {
        Err(io::Error::new(
            io::ErrorKind::NotFound,
            format!("{message}: {}", path.display()),
        ))
    }
}

fn print_usage() {
    println!(
        "usage: cargo xtask desktop-build-promote [--verify] [validation-label]\n\
         cargo xtask desktop-build-promote --check\n\
         The default path runs required packaging checks, incrementally builds, and promotes.\n\
         Pass --verify to also run the desktop and server-manager startup smoke.\n\
         Windows compilation uses the windows-desktop-dev CMake preset until its promotion backend is added."
    );
}

#[cfg(test)]
pub(super) fn parse_for_test(arguments: impl IntoIterator<Item = OsString>) -> io::Result<String> {
    Ok(format!("{:?}", parse(arguments)?))
}
