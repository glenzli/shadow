use super::catalog::print_stats;
use anyhow::{Context, Result};
use shadow_catalog::{
    CatalogBackupReceipt, CatalogBackupVerification, create_catalog_backup, verify_catalog_backup,
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

fn print_receipt(receipt: &CatalogBackupReceipt) {
    println!(
        "created verified catalog backup: {}",
        receipt.destination.display()
    );
    print_verification(&receipt.verification);
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
