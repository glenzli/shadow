use std::process::Command;

pub(super) fn run() {
    print_version("rustc", &["--version"]);
    print_version("cargo", &["--version"]);
    print_version("clang++", &["--version"]);
    print_version("cmake", &["--version"]);
    print_version("ninja", &["--version"]);
    print_version("pkg-config", &["--modversion", "libraw"]);
    print_version("qtpaths6", &["--version"]);
}

fn print_version(program: &str, arguments: &[&str]) {
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
