use std::path::Path;
use std::{fs, os::windows::process::CommandExt as _, process::Command};

#[test]
fn payloads_inherit_only_the_current_user_and_system_before_handle_transfer() {
    let root = tempfile::tempdir().unwrap();
    let directory = root.path().join("private-staging");
    fs::create_dir(&directory).unwrap();
    super::directory(&directory).unwrap();
    let payload = directory.join("samples.u16le");
    fs::write(&payload, b"synthetic samples").unwrap();
    assert_private_access(&directory, &payload, false);
    super::file(&payload).unwrap();
    assert_private_access(&directory, &payload, true);
}

fn assert_private_access(directory: &Path, payload: &Path, protected_file: bool) {
    // Inspect through Windows' independent ACL projection, not this owner's
    // SDDL construction or SDK internals. No production process invokes a shell.
    let result = Command::new("powershell.exe")
        .args(["-NoProfile", "-NonInteractive", "-Command", r#"
$ErrorActionPreference='Stop'
$user=[System.Security.Principal.WindowsIdentity]::GetCurrent().User.Value
foreach($path in @($env:SHADOW_PRIVATE_STAGING_TEST_ROOT,$env:SHADOW_PRIVATE_STAGING_TEST_FILE)) {
    $acl=if([System.IO.Directory]::Exists($path)){
        [System.IO.Directory]::GetAccessControl($path)
    }else{[System.IO.File]::GetAccessControl($path)}
    $hardened=$path -eq $env:SHADOW_PRIVATE_STAGING_TEST_ROOT -or
        $env:SHADOW_PRIVATE_STAGING_TEST_PROTECTED -eq 'true'
    if($hardened -and $acl.GetOwner([System.Security.Principal.SecurityIdentifier]).Value -ne $user){throw 'wrong owner'}
    if($hardened -and -not $acl.AreAccessRulesProtected){throw 'inherited broad ACL remains'}
    $rules=@($acl.GetAccessRules($true,$true,[System.Security.Principal.SecurityIdentifier]))
    if($rules.Count -ne 2){throw 'unexpected access rule count'}
    foreach($rule in $rules){
        if($rule.IdentityReference.Value -notin @($user,'S-1-5-18')){throw 'unexpected principal'}
        if($rule.AccessControlType -ne 'Allow' -or $rule.FileSystemRights -ne 'FullControl'){throw 'wrong access'}
    }
}
'owner-only Windows staging passed'
"#])
        .env("SHADOW_PRIVATE_STAGING_TEST_ROOT", &directory)
        .env("SHADOW_PRIVATE_STAGING_TEST_FILE", &payload)
        .env("SHADOW_PRIVATE_STAGING_TEST_PROTECTED", protected_file.to_string())
        .creation_flags(0x0800_0000)
        .output().unwrap();
    assert!(
        result.status.success(),
        "{}",
        String::from_utf8_lossy(&result.stderr)
    );
}

#[test]
fn linked_payloads_are_rejected_without_changing_source_bytes() {
    let root = tempfile::tempdir().unwrap();
    let original = root.path().join("original");
    let alias = root.path().join("alias");
    fs::write(&original, b"unrelated source").unwrap();
    fs::hard_link(&original, &alias).unwrap();
    assert!(super::file(&alias).is_err());
    assert_eq!(fs::read(&original).unwrap(), b"unrelated source");
}
