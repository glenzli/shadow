use std::{
    fs,
    os::unix::fs::PermissionsExt,
    path::Path,
    time::{Duration, Instant},
};

use super::{HelperExecution, execute_decode_helper_with_timeout};
use crate::isolated_proxy::route_fixture::safety_fixture;

#[test]
fn helper_timeout_kills_the_process_group_before_pipe_draining_can_block() {
    let (root, _source, helper) = safety_fixture("process-group-timeout");
    write_pipe_holding_helper(&helper);
    let started_at = Instant::now();
    let result = execute_decode_helper_with_timeout(&helper, Duration::from_millis(80), |_| {})
        .expect("execute pipe-holding helper");
    assert!(matches!(result, HelperExecution::TimedOut(_)));
    assert!(
        started_at.elapsed() < Duration::from_secs(1),
        "timeout must not wait for a descendant retaining stdout/stderr"
    );
    fs::remove_dir_all(root).expect("remove process-group timeout fixture");
}

fn write_pipe_holding_helper(path: &Path) {
    // `sleep` inherits both pipes. The shell is the direct child and waits
    // for it, so a timeout must kill the helper's *process group* or the
    // desktop would remain stuck draining a pipe after the shell dies.
    fs::write(path, "#!/bin/sh\nsleep 2 &\nwait\n").expect("write pipe-holding fake helper");
    let mut permissions = fs::metadata(path)
        .expect("read pipe-holding helper metadata")
        .permissions();
    permissions.set_mode(0o700);
    fs::set_permissions(path, permissions).expect("make pipe-holding helper executable");
}
