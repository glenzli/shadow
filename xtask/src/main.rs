mod coordination_health;
mod daily_use_smoke;
mod library_scale_smoke;
mod local_workspace_guard;

use std::{
    env, io,
    path::{Path, PathBuf},
    process::Command,
};

fn main() -> io::Result<()> {
    let command = env::args().nth(1).unwrap_or_else(|| "help".to_owned());
    match command.as_str() {
        "check" => {
            run("cargo", &["fmt", "--check"])?;
            run(
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
        "test" => run("cargo", &["test", "--workspace"]),
        "native-configure" => configure_preset("native-dev").map(|_| ()),
        "native-build" => build_preset("native-dev"),
        "desktop-build" => {
            let build_directory = configure_preset("desktop-dev")?;
            build_directory_contents(&build_directory)
        }
        "desktop-check" => {
            let build_directory = configure_preset("desktop-dev")?;
            build_directory_contents(&build_directory)?;
            run_ctest(&build_directory)
        }
        "desktop-release" => {
            let build_directory = configure_preset("desktop-release")?;
            build_directory_contents(&build_directory)
        }
        "native-check" => {
            let build_directory = configure_preset("native-dev")?;
            build_directory_contents(&build_directory)?;
            run_ctest(&build_directory)
        }
        "raw-smoke" => raw_smoke(env::args_os().nth(2)),
        "daily-use-smoke" => daily_use_smoke::run(env::args_os().nth(2)),
        "library-scale-smoke" => library_scale_smoke::run(env::args_os().skip(2)),
        "coordination-health" => coordination_health::run(env::args_os().skip(2)),
        "local-workspace-guard" => local_workspace_guard::run(env::args_os().skip(2)),
        "doctor" => {
            doctor("rustc", &["--version"]);
            doctor("cargo", &["--version"]);
            doctor("clang++", &["--version"]);
            doctor("cmake", &["--version"]);
            doctor("ninja", &["--version"]);
            doctor("pkg-config", &["--modversion", "libraw"]);
            doctor("qtpaths6", &["--version"]);
            Ok(())
        }
        _ => {
            println!(
                "cargo xtask <check|test|native-configure|native-build|native-check|desktop-build|desktop-check|desktop-release|raw-smoke [fixture-directory]|daily-use-smoke [fixture-directory]|library-scale-smoke [--photos N] [--page-size N]|coordination-health [--root PATH] [--stale-after-minutes N] [--fail-on-stale] [--strict] [--commit-gate] [--bulk-stage-gate]|local-workspace-guard [--root PATH]|doctor>"
            );
            Ok(())
        }
    }
}

fn raw_smoke(folder: Option<std::ffi::OsString>) -> io::Result<()> {
    let repository_root = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .expect("xtask lives directly below the repository root")
        .to_path_buf();
    let folder = folder.map_or_else(
        || repository_root.join("local-reference/sample-assets/raw"),
        |path| {
            let path = PathBuf::from(path);
            if path.is_absolute() {
                path
            } else {
                repository_root.join(path)
            }
        },
    );
    if !folder.is_dir() {
        return Err(io::Error::new(
            io::ErrorKind::NotFound,
            format!("RAW fixture directory does not exist: {}", folder.display()),
        ));
    }
    let folder = folder.canonicalize()?;

    let status = Command::new("cargo")
        .current_dir(repository_root)
        .env("SHADOW_TEST_RAW_FOLDER", &folder)
        .args([
            "test",
            "-p",
            "shadow-bridge",
            "real_raw_folder_smoke_matrix",
            "--",
            "--ignored",
            "--nocapture",
        ])
        .status()?;
    if status.success() {
        Ok(())
    } else {
        Err(io::Error::other(format!(
            "RAW smoke matrix exited with status {status}"
        )))
    }
}

fn run(program: &str, arguments: &[&str]) -> io::Result<()> {
    let status = Command::new(program).args(arguments).status()?;
    if status.success() {
        Ok(())
    } else {
        Err(io::Error::other(format!(
            "{program} exited with status {status}"
        )))
    }
}

fn configure_preset(preset: &str) -> io::Result<PathBuf> {
    let build_directory = preset_build_directory(preset)?;
    let status = Command::new("cmake")
        .args(["--preset", preset, "-B"])
        .arg(&build_directory)
        .status()?;
    if status.success() {
        Ok(build_directory)
    } else {
        Err(io::Error::other(format!(
            "cmake configure preset {preset} exited with status {status}"
        )))
    }
}

fn build_preset(preset: &str) -> io::Result<()> {
    let build_directory = preset_build_directory(preset)?;
    build_directory_contents(&build_directory)
}

fn build_directory_contents(build_directory: &Path) -> io::Result<()> {
    let status = Command::new("cmake")
        .args(["--build"])
        .arg(build_directory)
        .status()?;
    if status.success() {
        Ok(())
    } else {
        Err(io::Error::other(format!(
            "cmake build {} exited with status {status}",
            build_directory.display()
        )))
    }
}

fn run_ctest(build_directory: &Path) -> io::Result<()> {
    let status = Command::new("ctest")
        .args(["--test-dir"])
        .arg(build_directory)
        .arg("--output-on-failure")
        .status()?;
    if status.success() {
        Ok(())
    } else {
        Err(io::Error::other(format!(
            "ctest for {} exited with status {status}",
            build_directory.display()
        )))
    }
}

fn preset_build_directory(preset: &str) -> io::Result<PathBuf> {
    let repository_root = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .expect("xtask lives directly below the repository root")
        .canonicalize()?;
    let build_directory = env::var_os("SHADOW_BUILD_DIR").map_or_else(
        || {
            repository_root
                .parent()
                .expect("repository root has a parent directory")
                .join(".shadow-local-build")
                .join(preset)
        },
        PathBuf::from,
    );
    if !build_directory.is_absolute() || build_directory.starts_with(&repository_root) {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            format!(
                "SHADOW_BUILD_DIR must be an absolute path outside {}; received {}",
                repository_root.display(),
                build_directory.display()
            ),
        ));
    }
    Ok(build_directory)
}

fn doctor(program: &str, arguments: &[&str]) {
    match Command::new(program).args(arguments).output() {
        Ok(output) if output.status.success() => {
            let version = String::from_utf8_lossy(&output.stdout);
            println!(
                "{program}: {}",
                version.lines().next().unwrap_or("available")
            );
        }
        _ => println!("{program}: missing"),
    }
}
