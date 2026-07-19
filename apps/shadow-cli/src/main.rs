use std::{env, path::Path};

use anyhow::{Context, Result, bail};
use shadow_catalog::{Catalog, CatalogStats};
use shadow_core::scan_folder;

fn main() -> Result<()> {
    let arguments = env::args().skip(1).collect::<Vec<_>>();
    match arguments.as_slice() {
        [command, catalog_path] if command == "init" => {
            let catalog = open_catalog(catalog_path)?;
            println!("initialized catalog schema v{}", catalog.schema_version()?);
            print_stats(catalog.stats()?);
        }
        [command, catalog_path] if command == "stats" => {
            let catalog = open_catalog(catalog_path)?;
            print_stats(catalog.stats()?);
        }
        [command, catalog_path, folder] if command == "scan" => {
            let mut catalog = open_catalog(catalog_path)?;
            let report = scan_folder(&mut catalog, Path::new(folder))?;
            println!(
                "scan: seen={} supported={} inserted={} unchanged={} revalidate={} skipped={} issues={}",
                report.files_seen,
                report.supported_files,
                report.inserted,
                report.unchanged,
                report.needs_revalidation,
                report.skipped,
                report.issues.len()
            );
            for issue in report.issues {
                eprintln!("{}: {}", issue.path.display(), issue.message);
            }
            print_stats(catalog.stats()?);
        }
        _ => {
            print_usage();
            bail!("invalid arguments");
        }
    }

    Ok(())
}

fn open_catalog(path: &str) -> Result<Catalog> {
    let path = Path::new(path);
    if let Some(parent) = path
        .parent()
        .filter(|parent| !parent.as_os_str().is_empty())
    {
        std::fs::create_dir_all(parent)
            .with_context(|| format!("create catalog directory {}", parent.display()))?;
    }
    Catalog::open(path).with_context(|| format!("open catalog {}", path.display()))
}

fn print_stats(stats: CatalogStats) {
    println!(
        "catalog: photos={} representations={} locations={} needs_revalidation={}",
        stats.photos, stats.representations, stats.locations, stats.locations_needing_revalidation
    );
}

fn print_usage() {
    eprintln!(
        "usage:\n  shadow-cli init <catalog.sqlite>\n  shadow-cli scan <catalog.sqlite> <folder>\n  shadow-cli stats <catalog.sqlite>"
    );
}
