use std::{env, path::Path};

use anyhow::{Context, Result, bail};
use shadow_bridge::inspect_libraw;
use shadow_catalog::{CatalogActor, CatalogStats};
use shadow_core::{ScanReport, resume_scan, scan_folder};
use shadow_domain::{DecoderSnapshot, ImportSessionId};

fn main() -> Result<()> {
    let arguments = env::args().skip(1).collect::<Vec<_>>();
    match arguments.as_slice() {
        [command, catalog_path] if command == "init" => {
            let actor = open_catalog(catalog_path)?;
            let catalog = actor.handle();
            println!("initialized catalog schema v{}", catalog.schema_version()?);
            print_stats(catalog.stats()?);
        }
        [command, catalog_path] if command == "stats" => {
            let actor = open_catalog(catalog_path)?;
            let catalog = actor.handle();
            print_stats(catalog.stats()?);
        }
        [command, catalog_path] if command == "recoverable" => {
            let actor = open_catalog(catalog_path)?;
            let catalog = actor.handle();
            let sessions = catalog.unfinished_import_sessions()?;
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
        }
        [command, raw_path] if command == "inspect-raw" => {
            let snapshot = inspect_libraw(Path::new(raw_path))
                .with_context(|| format!("inspect RAW {raw_path}"))?;
            print_decoder_snapshot(&snapshot);
        }
        [command, catalog_path, folder] if command == "scan" => {
            let actor = open_catalog(catalog_path)?;
            let mut catalog = actor.handle();
            let report = scan_folder(&mut catalog, Path::new(folder))?;
            print_report(report);
            print_stats(catalog.stats()?);
        }
        [command, catalog_path, session_id] if command == "resume" => {
            let session_id: ImportSessionId = session_id
                .parse()
                .with_context(|| format!("parse import session id {session_id}"))?;
            let actor = open_catalog(catalog_path)?;
            let mut catalog = actor.handle();
            let report = resume_scan(&mut catalog, session_id)?;
            print_report(report);
            print_stats(catalog.stats()?);
        }
        _ => {
            print_usage();
            bail!("invalid arguments");
        }
    }

    Ok(())
}

fn print_report(report: ScanReport) {
    println!(
        "scan: session={} seen={} supported={} inserted={} unchanged={} revalidate={} skipped={} issues={}",
        report.session_id,
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
}

fn open_catalog(path: &str) -> Result<CatalogActor> {
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

fn print_stats(stats: CatalogStats) {
    println!(
        "catalog: photos={} representations={} locations={} needs_revalidation={}",
        stats.photos, stats.representations, stats.locations, stats.locations_needing_revalidation
    );
}

fn print_decoder_snapshot(snapshot: &DecoderSnapshot) {
    let metadata = &snapshot.metadata;
    let capabilities = &snapshot.capabilities;
    println!(
        "decoder: provider={} version={} dng_sdk={} rawspeed={} jpeg={}",
        snapshot.provider.id,
        snapshot.provider.version,
        snapshot.provider.dng_sdk,
        snapshot.provider.rawspeed,
        snapshot.provider.jpeg
    );
    println!(
        "camera: make={} model={} normalized={}/{} dng={}",
        metadata.make,
        metadata.model,
        metadata.normalized_make,
        metadata.normalized_model,
        metadata.dng_version.as_deref().unwrap_or("none")
    );
    println!(
        "raw: canvas={}x{} image={}x{} margins={},{},{},{} cfa={} bits={} black={} white={}",
        metadata.raw_dimensions.width,
        metadata.raw_dimensions.height,
        metadata.image_dimensions.width,
        metadata.image_dimensions.height,
        metadata.margins.left,
        metadata.margins.top,
        metadata.margins.right,
        metadata.margins.bottom,
        metadata.cfa_pattern,
        metadata.sensor_bits,
        metadata.black_level,
        metadata.white_level
    );
    println!(
        "capabilities: metadata={} previews={} mosaic={} reference_rgb={} pending_opcodes={:?}",
        capabilities.metadata.is_available(),
        capabilities.embedded_previews.is_available(),
        capabilities.mosaic.is_available(),
        capabilities.reference_rgb.is_available(),
        capabilities.pending_corrections.dng_opcode_list_bytes
    );
    for preview in &snapshot.previews {
        println!(
            "preview: id={} codec={} dimensions={}x{} bits={} channels={} bytes={} decodable={}",
            preview.provider_id,
            preview.codec.as_str(),
            preview.dimensions.width,
            preview.dimensions.height,
            preview.bits_per_channel,
            preview.channels,
            preview.encoded_bytes,
            preview.decodable
        );
    }
}

fn print_usage() {
    eprintln!(
        "usage:\n  shadow-cli init <catalog.sqlite>\n  shadow-cli scan <catalog.sqlite> <folder>\n  shadow-cli resume <catalog.sqlite> <session-id>\n  shadow-cli recoverable <catalog.sqlite>\n  shadow-cli stats <catalog.sqlite>\n  shadow-cli inspect-raw <path>"
    );
}
