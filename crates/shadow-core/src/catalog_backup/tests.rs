use std::{
    fs,
    sync::atomic::{AtomicU64, Ordering},
    time::{SystemTime, UNIX_EPOCH},
};

use shadow_catalog::CatalogActor;

use super::*;
use crate::{WorkScheduler, WorkSchedulerConfig};

static TEST_SEQUENCE: AtomicU64 = AtomicU64::new(0);

#[test]
fn policy_rejects_ambiguous_prefixes_and_zero_retention() {
    assert!(matches!(
        CatalogBackupPolicy::new("backups").with_file_prefix("../../outside"),
        Err(CatalogBackupPolicyError::InvalidFilePrefix)
    ));
    assert!(matches!(
        CatalogBackupPolicy::new("backups").with_retain_count(0),
        Err(CatalogBackupPolicyError::ZeroRetainCount)
    ));
}

#[test]
fn background_backup_is_verified_and_retention_never_removes_the_new_copy() {
    let root = temporary_test_root("background-backup");
    fs::create_dir_all(&root).expect("create test backup root");
    let source = root.join("catalog.sqlite");
    let actor = CatalogActor::spawn(&source).expect("create source catalog");
    actor
        .shutdown()
        .expect("close source catalog before backup");

    let scheduler =
        WorkScheduler::start(&WorkSchedulerConfig::new(1, 2)).expect("start background scheduler");
    let policy = CatalogBackupPolicy::new(root.join("backups"))
        .with_file_prefix("shadow-test")
        .expect("safe managed prefix")
        .with_retain_count(1)
        .expect("retain one backup");
    let service = CatalogBackupService::new(scheduler.handle(), policy);

    let first = completed_backup(service.try_schedule(&source).expect("queue first backup"));
    assert!(first.receipt.destination.is_file());
    assert_eq!(first.retention.removed, Vec::<PathBuf>::new());

    let first_file_name = first
        .receipt
        .destination
        .file_name()
        .expect("first backup must have a file name")
        .to_os_string();
    let second = completed_backup(service.try_schedule(&source).expect("queue second backup"));
    assert!(second.receipt.destination.is_file());
    assert_eq!(
        second
            .retention
            .removed
            .iter()
            .map(|path| {
                path.file_name()
                    .expect("removed backup must have a file name")
                    .to_os_string()
            })
            .collect::<Vec<_>>(),
        vec![first_file_name]
    );
    assert_eq!(second.retention.retained_count, 1);
    assert!(second.retention.warnings.is_empty());

    scheduler.shutdown().expect("shutdown background scheduler");
    fs::remove_dir_all(root).expect("remove test backup root");
}

fn completed_backup(job: CatalogBackupJob) -> CatalogBackupCompletion {
    match job.wait().expect("receive scheduled backup outcome") {
        WorkOutcome::Completed(Ok(completion)) => completion,
        outcome => panic!("expected successful completed backup, got {outcome:?}"),
    }
}

fn temporary_test_root(label: &str) -> PathBuf {
    let millis = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .expect("test clock after Unix epoch")
        .as_millis();
    let sequence = TEST_SEQUENCE.fetch_add(1, Ordering::Relaxed);
    std::env::temp_dir().join(format!(
        "shadow-core-{label}-{}-{millis}-{sequence}",
        std::process::id()
    ))
}
