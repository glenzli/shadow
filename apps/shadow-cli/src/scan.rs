use super::{catalog, decode::LibRawInspector};
use anyhow::Result;
use shadow_core::{
    DecodeInspectionActor, ScanReport, resume_scan, scan_folder, scan_folder_with_inspection,
};
use shadow_domain::ImportSessionId;
use std::path::Path;

pub(super) fn folder(catalog_path: &str, folder: &str) -> Result<()> {
    let actor = catalog::open(catalog_path)?;
    let mut catalog = actor.handle();
    let report = scan_folder(&mut catalog, Path::new(folder))?;
    print_report(report);
    catalog::print_stats(catalog.stats()?);
    Ok(())
}

pub(super) fn folder_with_cache(catalog_path: &str, cache_root: &str, folder: &str) -> Result<()> {
    let actor = catalog::open(catalog_path)?;
    let mut catalog = actor.handle();
    let inspector = DecodeInspectionActor::spawn_with_cache(
        catalog.clone(),
        LibRawInspector::new(),
        cache_root,
    )?;
    let report = scan_folder_with_inspection(&mut catalog, &inspector.handle(), Path::new(folder))?;
    inspector.shutdown()?;
    print_report(report);
    catalog::print_stats(catalog.stats()?);
    actor.shutdown()?;
    Ok(())
}

pub(super) fn resume(catalog_path: &str, session_id: ImportSessionId) -> Result<()> {
    let actor = catalog::open(catalog_path)?;
    let mut catalog = actor.handle();
    let report = resume_scan(&mut catalog, session_id)?;
    print_report(report);
    catalog::print_stats(catalog.stats()?);
    Ok(())
}

fn print_report(report: ScanReport) {
    println!(
        "scan: session={} seen={} supported={} inserted={} unchanged={} revalidate={} decode_queued={} skipped={} issues={}",
        report.session_id,
        report.files_seen,
        report.supported_files,
        report.inserted,
        report.unchanged,
        report.needs_revalidation,
        report.decode_inspections_queued,
        report.skipped,
        report.issues.len()
    );
    for issue in report.issues {
        eprintln!("{}: {}", issue.path.display(), issue.message);
    }
}
