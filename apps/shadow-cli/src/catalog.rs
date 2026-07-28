use anyhow::{Context, Result};
use shadow_catalog::{CatalogActor, CatalogStats};
use std::path::Path;

pub(super) fn initialize(path: &str) -> Result<()> {
    let actor = open(path)?;
    let catalog = actor.handle();
    println!("initialized catalog schema v{}", catalog.schema_version()?);
    print_stats(catalog.stats()?);
    Ok(())
}

pub(super) fn print_catalog_stats(path: &str) -> Result<()> {
    let actor = open(path)?;
    print_stats(actor.handle().stats()?);
    Ok(())
}

pub(super) fn print_recoverable_sessions(path: &str) -> Result<()> {
    let actor = open(path)?;
    let sessions = actor.handle().unfinished_import_sessions()?;
    if sessions.is_empty() {
        println!("no recoverable import sessions");
    }
    for session in sessions {
        println!(
            "{} state={} root={} updated_at_ms={}",
            session.id,
            session.state.as_str(),
            session.root.display_path,
            session.updated_at_ms
        );
    }
    Ok(())
}

pub(super) fn open(path: &str) -> Result<CatalogActor> {
    let path = Path::new(path);
    if let Some(parent) = path
        .parent()
        .filter(|parent| !parent.as_os_str().is_empty())
    {
        std::fs::create_dir_all(parent)
            .with_context(|| format!("create catalog directory {}", parent.display()))?;
    }
    CatalogActor::spawn(path).with_context(|| format!("open catalog {}", path.display()))
}

pub(super) fn print_stats(stats: CatalogStats) {
    println!(
        "catalog: photos={} representations={} locations={} needs_revalidation={}",
        stats.photos, stats.representations, stats.locations, stats.locations_needing_revalidation
    );
}
