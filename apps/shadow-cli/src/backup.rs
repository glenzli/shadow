use super::catalog::print_stats;
use anyhow::{Context, Result};
use shadow_catalog::{
    CatalogBackupReceipt, CatalogBackupVerification, CatalogRestoreReceipt, create_catalog_backup,
    restore_catalog_backup_offline, verify_catalog_backup,
};
use std::path::Path;

pub(super) fn create(catalog_path: &str, backup_path: &str) -> Result<()> {
    let receipt = create_catalog_backup(Path::new(catalog_path), Path::new(backup_path))
        .with_context(|| {
            format!("create verified catalog backup from {catalog_path} to {backup_path}")
        })?;
    print_receipt(&receipt);
    Ok(())
}

pub(super) fn verify(backup_path: &str) -> Result<()> {
    let verification = verify_catalog_backup(Path::new(backup_path))
        .with_context(|| format!("verify catalog backup {backup_path}"))?;
    println!("verified catalog backup: {backup_path}");
    print_verification(&verification);
    Ok(())
}

pub(super) fn restore(backup_path: &str, catalog_path: &str) -> Result<()> {
    let receipt = restore_catalog_backup_offline(Path::new(backup_path), Path::new(catalog_path))
        .with_context(|| {
        format!("restore verified catalog backup {backup_path} over offline Catalog {catalog_path}")
    })?;
    print_restore_receipt(&receipt);
    Ok(())
}

fn print_receipt(receipt: &CatalogBackupReceipt) {
    println!(
        "created verified catalog backup: {}",
        receipt.destination.display()
    );
    print_verification(&receipt.verification);
}

fn print_restore_receipt(receipt: &CatalogRestoreReceipt) {
    println!(
        "restored verified Catalog: {}",
        receipt.destination.display()
    );
    if let Some(rollback) = receipt.rollback_directory.as_ref() {
        println!(
            "previous Catalog retained for rollback: {}",
            rollback.display()
        );
    } else {
        println!("restore created a new Catalog; no previous file set existed");
    }
    print_verification(&receipt.restored_verification);
}

fn print_verification(verification: &CatalogBackupVerification) {
    let revision = verification.schema_version;
    println!(
        "backup: schema_revision={}.{} pages={} page_size={} bytes={}",
        revision / 100,
        revision % 100,
        verification.page_count,
        verification.page_size,
        verification.database_bytes
    );
    print_stats(verification.stats);
}
