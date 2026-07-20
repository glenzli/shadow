use std::{
    env,
    path::{Path, PathBuf},
    time::{SystemTime, UNIX_EPOCH},
};

use anyhow::{Context, Result, bail};
use shadow_bridge::{
    extract_best_libraw_preview, inspect_libraw, libraw_provider_version,
    render_libraw_reference_proxy,
};
use shadow_catalog::{CatalogActor, CatalogStats, RegisterAsset};
use shadow_core::{
    DecodeInspectionActor, DecodeInspectionOutcome, DecodeInspectionRequest, DecodeInspector,
    PreviewCacheOutcome, ScanReport, fingerprint_source, native_location, resume_scan, scan_folder,
    scan_folder_with_inspection,
};
use shadow_domain::{
    DecoderSnapshot, ImportSessionId, PreviewPayload, ProxyPayload, RepresentationKind,
};

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
        [command, catalog_path, cache_root, raw_path] if command == "inspect-store" => {
            inspect_store(catalog_path, cache_root, raw_path)?;
        }
        [command, catalog_path, folder] if command == "scan" => {
            let actor = open_catalog(catalog_path)?;
            let mut catalog = actor.handle();
            let report = scan_folder(&mut catalog, Path::new(folder))?;
            print_report(report);
            print_stats(catalog.stats()?);
        }
        [command, catalog_path, cache_root, folder] if command == "scan-cache" => {
            scan_cache(catalog_path, cache_root, folder)?;
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

fn inspect_store(catalog_path: &str, cache_root: &str, raw_path: &str) -> Result<()> {
    let raw_path = absolute_path(Path::new(raw_path))?;
    let source = fingerprint_source(&raw_path)
        .with_context(|| format!("read RAW metadata {}", raw_path.display()))?;
    let actor = open_catalog(catalog_path)?;
    let catalog = actor.handle();
    let registered = catalog.register_asset(&RegisterAsset {
        kind: RepresentationKind::OriginalRaw,
        location: native_location(&raw_path),
        byte_len: source.byte_len,
        modified_at_ms: source.modified_at_ms,
        now_ms: now_ms(),
    })?;
    let inspector = DecodeInspectionActor::spawn_with_cache(
        catalog.clone(),
        LibRawInspector::new(),
        cache_root,
    )?;
    let outcome = inspector
        .handle()
        .submit(DecodeInspectionRequest {
            representation_id: registered.representation_id,
            path: raw_path,
            expected_source: source,
        })?
        .wait()?;
    print_inspection_outcome(&outcome);
    if matches!(outcome, DecodeInspectionOutcome::Recorded { .. }) {
        for record in catalog.decode_snapshots(registered.representation_id)? {
            print_decoder_snapshot(&record.snapshot);
        }
    }
    inspector.shutdown()?;
    actor.shutdown()?;
    Ok(())
}

fn scan_cache(catalog_path: &str, cache_root: &str, folder: &str) -> Result<()> {
    let actor = open_catalog(catalog_path)?;
    let mut catalog = actor.handle();
    let inspector = DecodeInspectionActor::spawn_with_cache(
        catalog.clone(),
        LibRawInspector::new(),
        cache_root,
    )?;
    let report = scan_folder_with_inspection(&mut catalog, &inspector.handle(), Path::new(folder))?;
    inspector.shutdown()?;
    print_report(report);
    print_stats(catalog.stats()?);
    actor.shutdown()?;
    Ok(())
}

fn absolute_path(path: &Path) -> Result<PathBuf> {
    if path.is_absolute() {
        Ok(path.to_path_buf())
    } else {
        Ok(env::current_dir()
            .context("resolve current directory")?
            .join(path))
    }
}

fn print_inspection_outcome(outcome: &DecodeInspectionOutcome) {
    match outcome {
        DecodeInspectionOutcome::Recorded {
            provider_id,
            provider_version,
            preview,
        } => {
            println!("stored decoder snapshot: provider={provider_id} version={provider_version}");
            match preview {
                PreviewCacheOutcome::StoredEmbeddedPreview {
                    digest_hex,
                    byte_len,
                } => println!("cached embedded preview: blake3={digest_hex} bytes={byte_len}"),
                PreviewCacheOutcome::StoredGeneratedProxy {
                    digest_hex,
                    byte_len,
                    dimensions,
                } => println!(
                    "cached generated proxy: blake3={digest_hex} bytes={byte_len} dimensions={}x{}",
                    dimensions.width, dimensions.height
                ),
                PreviewCacheOutcome::NoVisualAvailable => {
                    println!(
                        "cached visual: neither embedded preview nor generated proxy available"
                    );
                }
                PreviewCacheOutcome::NotRequested => {}
                PreviewCacheOutcome::Discarded(reason) => {
                    println!("discarded embedded preview: reason={reason:?}");
                }
                PreviewCacheOutcome::Failed(message) => {
                    println!("embedded preview cache failed: {message}");
                }
            }
        }
        DecodeInspectionOutcome::Discarded(reason) => {
            println!("discarded decoder snapshot: reason={reason:?}");
        }
    }
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
        "usage:\n  shadow-cli init <catalog.sqlite>\n  shadow-cli scan <catalog.sqlite> <folder>\n  shadow-cli scan-cache <catalog.sqlite> <cache-root> <folder>\n  shadow-cli resume <catalog.sqlite> <session-id>\n  shadow-cli recoverable <catalog.sqlite>\n  shadow-cli stats <catalog.sqlite>\n  shadow-cli inspect-raw <path>\n  shadow-cli inspect-store <catalog.sqlite> <cache-root> <path>"
    );
}

#[derive(Debug, Clone)]
struct LibRawInspector {
    version: String,
}

impl LibRawInspector {
    fn new() -> Self {
        Self {
            version: libraw_provider_version(),
        }
    }
}

impl DecodeInspector for LibRawInspector {
    fn provider_id(&self) -> &'static str {
        "libraw"
    }

    fn provider_version(&self) -> &str {
        &self.version
    }

    fn inspect(&mut self, path: &Path) -> Result<DecoderSnapshot, String> {
        inspect_libraw(path).map_err(|error| error.to_string())
    }

    fn extract_best_preview(&mut self, path: &Path) -> Result<Option<PreviewPayload>, String> {
        extract_best_libraw_preview(path).map_err(|error| error.to_string())
    }

    fn render_proxy(&mut self, path: &Path) -> Result<Option<ProxyPayload>, String> {
        render_libraw_reference_proxy(path, 2_048, 88)
            .map(Some)
            .map_err(|error| error.to_string())
    }

    fn proxy_variant_key(&self) -> &'static str {
        "libraw:grid-jpeg-2048-q88-v1"
    }
}

fn now_ms() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .ok()
        .and_then(|duration| i64::try_from(duration.as_millis()).ok())
        .unwrap_or_default()
}
