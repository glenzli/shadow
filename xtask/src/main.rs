mod daily_use_smoke;

use std::{env, io, path::PathBuf, process::Command};

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
        "native-configure" => run("cmake", &["--preset", "native-dev"]),
        "native-build" => run("cmake", &["--build", "--preset", "native-dev"]),
        "desktop-build" => {
            run("cmake", &["--preset", "desktop-dev"])?;
            run("cmake", &["--build", "--preset", "desktop-dev"])
        }
        "desktop-check" => {
            run("cmake", &["--preset", "desktop-dev"])?;
            run("cmake", &["--build", "--preset", "desktop-dev"])?;
            run(
                "ctest",
                &["--test-dir", "build/desktop-dev", "--output-on-failure"],
            )
        }
        "desktop-release" => {
            run("cmake", &["--preset", "desktop-release"])?;
            run("cmake", &["--build", "--preset", "desktop-release"])
        }
        "native-check" => {
            run("cmake", &["--preset", "native-dev"])?;
            run("cmake", &["--build", "--preset", "native-dev"])?;
            run("ctest", &["--preset", "native-dev"])
        }
        "raw-smoke" => raw_smoke(env::args_os().nth(2)),
        "daily-use-smoke" => daily_use_smoke::run(env::args_os().nth(2)),
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
                "cargo xtask <check|test|native-configure|native-build|native-check|desktop-build|desktop-check|desktop-release|raw-smoke [fixture-directory]|daily-use-smoke [fixture-directory]|doctor>"
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
