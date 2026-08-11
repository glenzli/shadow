use std::{
    ffi::OsStr,
    fs::File,
    io::{self, Write},
    path::Path,
    process::{Command, Stdio},
};

pub(super) fn run(command: &mut Command, label: &str) -> io::Result<()> {
    let status = command.status()?;
    if status.success() {
        Ok(())
    } else {
        Err(io::Error::other(format!(
            "{label} exited with status {status}"
        )))
    }
}

pub(super) fn capture(command: &mut Command, label: &str) -> io::Result<String> {
    let output = command.output()?;
    if !output.status.success() {
        return Err(io::Error::other(format!(
            "{label} exited with status {}: {}",
            output.status,
            String::from_utf8_lossy(&output.stderr).trim()
        )));
    }
    Ok(String::from_utf8_lossy(&output.stdout).trim().to_owned())
}

pub(super) fn capture_with_input(
    program: impl AsRef<OsStr>,
    arguments: &[&str],
    input: &[u8],
    label: &str,
) -> io::Result<String> {
    let mut child = Command::new(program)
        .args(arguments)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()?;
    child
        .stdin
        .take()
        .ok_or_else(|| io::Error::other("child stdin was not piped"))?
        .write_all(input)?;
    let output = child.wait_with_output()?;
    if !output.status.success() {
        return Err(io::Error::other(format!(
            "{label} exited with status {}: {}",
            output.status,
            String::from_utf8_lossy(&output.stderr).trim()
        )));
    }
    Ok(String::from_utf8_lossy(&output.stdout).trim().to_owned())
}

pub(super) fn sha256_file(path: &Path) -> io::Result<String> {
    let output = capture(
        Command::new("shasum").args(["-a", "256"]).arg(path),
        &format!("hash {}", path.display()),
    )?;
    digest_token(&output)
}

pub(super) fn sha256_bytes(bytes: &[u8]) -> io::Result<String> {
    let output = capture_with_input("shasum", &["-a", "256"], bytes, "hash input")?;
    digest_token(&output)
}

pub(super) fn append_log(path: &Path) -> io::Result<(File, File)> {
    let stdout = File::options().create(true).append(true).open(path)?;
    let stderr = stdout.try_clone()?;
    Ok((stdout, stderr))
}

#[cfg(unix)]
pub(super) fn is_executable(path: &Path) -> bool {
    use std::os::unix::fs::PermissionsExt;

    path.metadata()
        .is_ok_and(|metadata| metadata.is_file() && metadata.permissions().mode() & 0o111 != 0)
}

#[cfg(not(unix))]
pub(super) fn is_executable(path: &Path) -> bool {
    path.is_file()
}

fn digest_token(output: &str) -> io::Result<String> {
    output
        .split_whitespace()
        .next()
        .filter(|digest| digest.len() == 64 && digest.bytes().all(|byte| byte.is_ascii_hexdigit()))
        .map(str::to_owned)
        .ok_or_else(|| io::Error::other(format!("invalid SHA-256 output: {output:?}")))
}
