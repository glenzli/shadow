use super::catalog;
use anyhow::{Context, Result, bail};
use shadow_bridge::{
    extract_best_libraw_preview, inspect_libraw, libraw_provider_version,
    render_libraw_reference_proxy,
};
use shadow_catalog::RegisterAsset;
use shadow_core::{
    CachedArtifactLoader, DecodeInspectionActor, DecodeInspectionOutcome, DecodeInspectionRequest,
    DecodeInspector, PreviewCacheOutcome, fingerprint_source, native_location,
};
use shadow_domain::{DecoderSnapshot, PreviewPayload, ProxyPayload, RepresentationKind};
use std::{
    env,
    path::{Path, PathBuf},
    time::{SystemTime, UNIX_EPOCH},
};

pub(super) fn inspect_raw(raw_path: &str) -> Result<()> {
    let snapshot =
        inspect_libraw(Path::new(raw_path)).with_context(|| format!("inspect RAW {raw_path}"))?;
    print_snapshot(&snapshot);
    Ok(())
}

pub(super) fn inspect_store(catalog_path: &str, cache_root: &str, raw_path: &str) -> Result<()> {
    let raw_path = absolute_path(Path::new(raw_path))?;
    let source = fingerprint_source(&raw_path)
        .with_context(|| format!("read RAW metadata {}", raw_path.display()))?;
    let actor = catalog::open(catalog_path)?;
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
            print_snapshot(&record.snapshot);
        }
    }
    inspector.shutdown()?;
    actor.shutdown()?;
    Ok(())
}

pub(super) fn read_cache(catalog_path: &str, cache_root: &str, raw_path: &str) -> Result<()> {
    let raw_path = absolute_path(Path::new(raw_path))?;
    let source = fingerprint_source(&raw_path)
        .with_context(|| format!("read RAW metadata {}", raw_path.display()))?;
    let actor = catalog::open(catalog_path)?;
    let catalog = actor.handle();
    let registered = catalog.register_asset(&RegisterAsset {
        kind: RepresentationKind::OriginalRaw,
        location: native_location(&raw_path),
        byte_len: source.byte_len,
        modified_at_ms: source.modified_at_ms,
        now_ms: now_ms(),
    })?;
    let artifacts = catalog.cached_artifacts(registered.representation_id)?;
    if artifacts.is_empty() {
        bail!("no cached visual for {}", raw_path.display());
    }
    let loader = CachedArtifactLoader::open(catalog.clone(), cache_root)?;
    for record in artifacts {
        let bytes = loader.load_bytes(&record)?;
        println!(
            "verified cached visual: role={} variant={} bytes={} dimensions={}x{}",
            record.artifact.role.as_str(),
            record.artifact.variant_key,
            bytes.len(),
            record.artifact.dimensions.width,
            record.artifact.dimensions.height
        );
    }
    actor.shutdown()?;
    Ok(())
}

#[derive(Debug, Clone)]
pub(super) struct LibRawInspector {
    version: String,
}

impl LibRawInspector {
    pub(super) fn new() -> Self {
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
        "libraw:grid-jpeg-2048-q95-444-v1"
    }
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
                PreviewCacheOutcome::PublishedEmbeddedPreview { byte_len } => {
                    println!("published session embedded preview: bytes={byte_len}");
                }
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

fn print_snapshot(snapshot: &DecoderSnapshot) {
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
        "capabilities: metadata={} previews={} raw_frame={} reference_rgb={} pending_opcodes={:?}",
        capabilities.metadata.is_available(),
        capabilities.embedded_previews.is_available(),
        capabilities.raw_frame.is_available(),
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

fn now_ms() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .ok()
        .and_then(|duration| i64::try_from(duration.as_millis()).ok())
        .unwrap_or_default()
}
