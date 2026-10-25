//! Private Windows credential fixture; keep platform FFI outside shadow-ai.
use base64::{Engine as _, engine::general_purpose::STANDARD};
use std::{os::windows::process::CommandExt, path::Path, process::Command};

pub(super) fn protect(path: &Path) {
    let quoted = path.to_str().expect("fixture path").replace('\'', "''");
    let script = format!(
        r#"
$ErrorActionPreference = 'Stop'
$path = '{quoted}'
$user = [Security.Principal.WindowsIdentity]::GetCurrent().User
$system = [Security.Principal.SecurityIdentifier]::new('S-1-5-18')
$acl = [Security.AccessControl.FileSecurity]::new()
$acl.SetOwner($user)
$acl.SetAccessRuleProtection($true, $false)
foreach ($sid in @($user, $system)) {{
    $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($sid, 'FullControl', 'Allow'))
}}
[IO.FileInfo]::new($path).SetAccessControl($acl)
"#
    );
    // EncodedCommand takes UTF-16LE code. The only dynamic value is a quoted
    // fixture path; credentials are never passed to the helper process.
    let encoded = STANDARD.encode(
        script
            .encode_utf16()
            .flat_map(u16::to_le_bytes)
            .collect::<Vec<_>>(),
    );
    let output = Command::new("powershell.exe")
        .args([
            "-NoLogo",
            "-NoProfile",
            "-NonInteractive",
            "-EncodedCommand",
            &encoded,
        ])
        .creation_flags(0x0800_0000)
        .output()
        .expect("Windows private credential fixture helper");
    assert!(
        output.status.success(),
        "private fixture ACL: {}",
        String::from_utf8_lossy(&output.stderr)
    );
}
