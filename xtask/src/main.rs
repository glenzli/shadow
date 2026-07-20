use std::{env, io, process::Command};

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
        "native-check" => {
            run("cmake", &["--preset", "native-dev"])?;
            run("cmake", &["--build", "--preset", "native-dev"])?;
            run("ctest", &["--preset", "native-dev"])
        }
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
                "cargo xtask <check|test|native-configure|native-build|native-check|desktop-build|doctor>"
            );
            Ok(())
        }
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
