use std::{
    io,
    process::{Command, Output},
};

pub(super) fn run_checked(command: &mut Command, context: &str) -> io::Result<()> {
    let output = command.output()?;
    emit_process_output(context, &output);
    if output.status.success() {
        return Ok(());
    }
    Err(process_failure(context, &output))
}

pub(super) fn run_desktop_checked(command: &mut Command, context: &str) -> io::Result<()> {
    let output = command.output()?;
    emit_process_output(context, &output);
    if !output.status.success() {
        return Err(process_failure(context, &output));
    }
    let diagnostics = unexpected_desktop_diagnostics(&output);
    if diagnostics.is_empty() {
        return Ok(());
    }
    Err(io::Error::other(format!(
        "{context} emitted unexpected desktop runtime diagnostics:\n{}",
        diagnostics.join("\n")
    )))
}

fn process_failure(context: &str, output: &Output) -> io::Error {
    io::Error::other(format!(
        "{context} exited with status {}\n{}",
        output.status,
        output_summary(output)
    ))
}

fn emit_process_output(context: &str, output: &Output) {
    if output.stdout.is_empty() && output.stderr.is_empty() {
        return;
    }
    println!("{context} process output:\n{}", output_summary(output));
}

fn unexpected_desktop_diagnostics(output: &Output) -> Vec<String> {
    let stderr = String::from_utf8_lossy(&output.stderr);
    let stdout = String::from_utf8_lossy(&output.stdout);
    unexpected_desktop_diagnostic_lines(&format!("{stderr}\n{stdout}"))
}

fn unexpected_desktop_diagnostic_lines(output: &str) -> Vec<String> {
    output
        .lines()
        .map(str::trim)
        .filter(|line| !line.is_empty() && is_unexpected_desktop_diagnostic(line))
        .take(16)
        .map(ToOwned::to_owned)
        .collect()
}

fn is_unexpected_desktop_diagnostic(line: &str) -> bool {
    [
        "QQmlApplicationEngine failed",
        "QQmlComponent: Component is not ready",
        "Failed to load component",
        "ReferenceError:",
        "TypeError:",
        "Binding loop detected",
        "Cannot assign",
        "Unable to assign",
        "QObject::connect:",
    ]
    .iter()
    .any(|signature| line.contains(signature))
        || (line.contains("module \"") && line.contains("is not installed"))
}

fn output_summary(output: &Output) -> String {
    const MAX_BYTES: usize = 8 * 1024;
    let stderr = String::from_utf8_lossy(&output.stderr);
    let stdout = String::from_utf8_lossy(&output.stdout);
    let combined = format!("stderr:\n{stderr}\nstdout:\n{stdout}");
    if combined.len() <= MAX_BYTES {
        combined
    } else {
        format!("…{}", utf8_tail(&combined, MAX_BYTES))
    }
}

fn utf8_tail(value: &str, maximum_bytes: usize) -> &str {
    let mut start = value.len().saturating_sub(maximum_bytes);
    while start < value.len() && !value.is_char_boundary(start) {
        start += 1;
    }
    &value[start..]
}

#[cfg(test)]
mod tests;
