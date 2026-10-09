//! Bounded file/stdin transport for the shared headless project service.

use std::{
    ffi::OsString,
    fs::File,
    io::{self, Read, Write},
    path::Path,
};

use shadow_desktop_bridge::headless_project::{MAX_REQUEST_BYTES, Response, execute_json};

pub(super) fn run(arguments: &[OsString]) -> i32 {
    let response = match arguments {
        [root, request_path] => match read_request(request_path) {
            Ok(bytes) => execute_json(Path::new(root), &bytes),
            Err(error) => Response::failure("request_io_error", error.to_string()),
        },
        _ => Response::failure(
            "invalid_arguments",
            "usage: shadow-cli project <project-root> <request.json|->",
        ),
    };
    let status = if response.ok { 0 } else { 2 };
    let mut stdout = io::stdout().lock();
    if serde_json::to_writer(&mut stdout, &response).is_err() || writeln!(stdout).is_err() {
        eprintln!(
            "could not write the project response; the operation may already have completed. \
             Before retrying, inspect snapshot/history for edits, decision/decision_history for \
             review decisions, or the requested destination for preview/export. \
             Requests have no deduplication."
        );
        return 3;
    }
    status
}

fn read_request(path: &OsString) -> io::Result<Vec<u8>> {
    let input: Box<dyn Read> = if path == "-" {
        Box::new(io::stdin())
    } else {
        Box::new(File::open(path)?)
    };
    let mut bytes = Vec::new();
    input
        .take(MAX_REQUEST_BYTES as u64 + 1)
        .read_to_end(&mut bytes)?;
    Ok(bytes)
}
