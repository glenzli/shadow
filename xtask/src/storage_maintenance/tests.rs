use std::{
    ffi::OsString,
    fs,
    path::PathBuf,
    time::{Duration, SystemTime},
};

use super::{
    PruneRequest, Release, StoragePaths, parse_prune_request, prune_candidates, remove_candidates,
};

fn release(name: &str, age_seconds: u64) -> Release {
    Release {
        name: name.to_owned(),
        path: PathBuf::from(format!("/releases/{name}")),
        modified: SystemTime::UNIX_EPOCH + Duration::from_secs(age_seconds),
        bytes: age_seconds,
    }
}

#[test]
fn prune_plan_keeps_current_and_the_newest_requested_releases() {
    let candidates = prune_candidates(
        vec![
            release("old", 1),
            release("current", 2),
            release("recent", 3),
            release("newest", 4),
        ],
        "current",
        3,
    );
    assert_eq!(
        candidates
            .into_iter()
            .map(|release| release.name)
            .collect::<Vec<_>>(),
        vec!["old"]
    );
}

#[test]
fn prune_arguments_default_to_a_dry_run_and_reject_zero_retention() {
    assert_eq!(
        parse_prune_request(Vec::<OsString>::new()).expect("parse default"),
        PruneRequest::Run {
            keep: 3,
            apply: false,
        }
    );
    assert_eq!(
        parse_prune_request([
            OsString::from("--keep"),
            OsString::from("4"),
            OsString::from("--apply")
        ])
        .expect("parse explicit request"),
        PruneRequest::Run {
            keep: 4,
            apply: true,
        }
    );
    assert!(parse_prune_request([OsString::from("--keep"), OsString::from("0")]).is_err());
}

#[cfg(unix)]
#[test]
fn apply_removes_only_the_planned_direct_release() {
    let root =
        std::env::temp_dir().join(format!("shadow-storage-maintenance-{}", std::process::id()));
    let release_root = root.join("releases/debug");
    let current = release_root.join("current");
    let old = release_root.join("old");
    fs::create_dir_all(&current).expect("create current release");
    fs::create_dir_all(&old).expect("create old release");
    fs::write(current.join("marker"), b"current").expect("write current marker");
    fs::write(old.join("marker"), b"old").expect("write old marker");
    let current_link = root.join("current-debug");
    #[cfg(unix)]
    std::os::unix::fs::symlink("releases/debug/current", &current_link)
        .expect("link current release");

    let paths = StoragePaths {
        debug_release_root: release_root.clone(),
        current_debug_link: current_link,
        canonical_build_directory: root.join("canonical-build"),
        cargo_target_root: root.join("cargo-target"),
    };
    let candidate = Release {
        name: "old".to_owned(),
        path: old.clone(),
        modified: SystemTime::UNIX_EPOCH,
        bytes: 3,
    };
    remove_candidates(&paths, "current", &[candidate]).expect("remove planned old release");
    assert!(current.is_dir());
    assert!(!old.exists());
    fs::remove_dir_all(root).expect("remove temporary fixture");
}
