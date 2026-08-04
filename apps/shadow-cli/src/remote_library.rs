use std::{
    fs,
    net::SocketAddr,
    path::Path,
    str::FromStr,
    time::{SystemTime, UNIX_EPOCH},
};

use anyhow::{Context, Result, bail};
use shadow_cache::ContentAddressedStore;
use shadow_catalog::{
    ContentIdentity, RecordRepresentationContentIdentity,
    RecordRepresentationContentIdentityStatus, RegisterAsset,
};
use shadow_core::{fingerprint_source, native_location};
use shadow_desktop_bridge::{
    LibraryServerService, LibraryServerStartRequest, LibraryServerStorage,
};
use shadow_domain::{PhotoId, RepresentationId, RepresentationKind};
use shadow_library_sharing::{
    AuthorizationToken, LibraryClient, LibraryClientConfig, MirroredLocalSource,
    OriginalMaterializer, OriginalMaterializerPolicy, RemoteLibraryMirror,
};

use super::catalog;

pub(super) struct ServeOptions<'a> {
    pub catalog_path: &'a str,
    pub cache_root: &'a str,
    pub folder: &'a str,
    pub server_state_root: &'a str,
    pub bind_address: &'a str,
    pub token_file: &'a str,
    pub display_name: &'a str,
}

pub(super) fn serve(options: &ServeOptions<'_>) -> Result<()> {
    let bind_address = SocketAddr::from_str(options.bind_address)
        .with_context(|| format!("parse bind address {}", options.bind_address))?;
    let service = LibraryServerService::new(LibraryServerStorage {
        catalog_path: options.catalog_path.into(),
        preview_cache_root: options.cache_root.into(),
        state_root: options.server_state_root.into(),
    });
    let snapshot = service.start(LibraryServerStartRequest {
        bind_address,
        authorization: token_from_file(options.token_file)?,
        display_name: options.display_name.to_owned(),
        share_roots: vec![options.folder.into()],
        serves_originals: true,
    })?;
    println!(
        "remote Library ready: address={} folder={} preview_policy=embedded-first/generated-fallback private_provider={} provider_host={provider_mode}",
        snapshot
            .local_address
            .context("running Library server did not report a local address")?,
        options.folder,
        snapshot.provider_mode == "private",
        provider_mode = snapshot.provider_mode,
    );
    println!("press Ctrl-C to stop sharing");
    let _service = service;
    loop {
        std::thread::park();
    }
}

pub(super) fn sync(
    server_address: &str,
    token_file: &str,
    mirror_root: &str,
    preview_cache_root: &str,
) -> Result<()> {
    let client = client(server_address, token_file)?;
    let preview_store = ContentAddressedStore::open(preview_cache_root)?;
    let mut mirror = RemoteLibraryMirror::open(mirror_root)?;
    let report = mirror.sync(&client, &preview_store)?;
    let server = mirror
        .snapshot()
        .server
        .as_ref()
        .context("remote Library sync did not retain server identity")?;
    println!(
        "remote Library synchronized: server={} id={:?} pages={} photos={} previews_downloaded={} removed={}",
        server.display_name,
        server.server_id,
        report.page_count,
        report.photo_count,
        report.downloaded_previews,
        report.removed
    );
    Ok(())
}

pub(super) struct MaterializeOptions<'a> {
    pub server_address: &'a str,
    pub token_file: &'a str,
    pub mirror_root: &'a str,
    pub original_cache_root: &'a str,
    pub local_catalog_path: &'a str,
    pub remote_photo_id: &'a str,
    pub remote_representation_id: &'a str,
}

pub(super) fn materialize(options: &MaterializeOptions<'_>) -> Result<()> {
    let remote_photo_id = PhotoId::from_str(options.remote_photo_id)
        .with_context(|| format!("parse remote photo id {}", options.remote_photo_id))?;
    let remote_representation_id = RepresentationId::from_str(options.remote_representation_id)
        .with_context(|| {
            format!(
                "parse remote representation id {}",
                options.remote_representation_id
            )
        })?;
    let mut mirror = RemoteLibraryMirror::open(options.mirror_root)?;
    let remote_manifest = mirror
        .snapshot()
        .photos
        .iter()
        .find(|photo| {
            photo.manifest.photo_id == remote_photo_id
                && photo.manifest.representation_id == remote_representation_id
        })
        .map(|photo| photo.manifest.clone())
        .ok_or_else(|| {
            anyhow::anyhow!(
                "remote photo {remote_photo_id}/{remote_representation_id} is not in the local mirror"
            )
        })?;
    let client = client(options.server_address, options.token_file)?;
    let original_cache = OriginalMaterializer::open(
        options.original_cache_root,
        OriginalMaterializerPolicy::default(),
    )?;
    let local_original =
        original_cache.materialize(&client, remote_photo_id, remote_representation_id)?;

    let actor = catalog::open(options.local_catalog_path)?;
    let local_catalog = actor.handle();
    let source = fingerprint_source(&local_original.path).with_context(|| {
        format!(
            "read materialized source metadata {}",
            local_original.path.display()
        )
    })?;
    if source.byte_len != local_original.manifest.byte_len {
        bail!("materialized source size changed before Catalog registration");
    }
    let registered = local_catalog.register_asset(&RegisterAsset {
        kind: RepresentationKind::OriginalRaw,
        location: native_location(&local_original.path),
        byte_len: source.byte_len,
        modified_at_ms: source.modified_at_ms,
        now_ms: now_ms(),
    })?;
    let identity_status = local_catalog.record_representation_content_identity(
        &RecordRepresentationContentIdentity {
            representation_id: registered.representation_id,
            expected_source: source,
            identity: ContentIdentity::whole_file_blake3(local_original.manifest.digest_blake3),
            observed_at_ms: now_ms(),
        },
    )?;
    if identity_status != RecordRepresentationContentIdentityStatus::Recorded {
        bail!("materialized source changed before content identity registration");
    }
    actor.shutdown()?;

    mirror.mark_materialized(
        remote_photo_id,
        remote_representation_id,
        MirroredLocalSource::for_remote_manifest(
            registered.photo_id,
            registered.representation_id,
            local_original.manifest.digest_blake3,
            native_path_text(&local_original.path),
            &remote_manifest,
        ),
    )?;
    println!(
        "remote original ready for local editing: remote={}/{} local={}/{} bytes={} reused={} path={}",
        remote_photo_id,
        remote_representation_id,
        registered.photo_id,
        registered.representation_id,
        local_original.manifest.byte_len,
        local_original.reused_existing,
        local_original.path.display()
    );
    Ok(())
}

fn client(server_address: &str, token_file: &str) -> Result<LibraryClient> {
    let server_address = SocketAddr::from_str(server_address)
        .with_context(|| format!("parse server address {server_address}"))?;
    Ok(LibraryClient::new(LibraryClientConfig::new(
        server_address,
        token_from_file(token_file)?,
    )))
}

fn token_from_file(path: &str) -> Result<AuthorizationToken> {
    let value = fs::read_to_string(path).with_context(|| format!("read sharing token {path}"))?;
    AuthorizationToken::parse(value.trim().to_owned()).map_err(Into::into)
}

fn native_path_text(path: &Path) -> String {
    path.to_string_lossy().into_owned()
}

fn now_ms() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .ok()
        .and_then(|duration| i64::try_from(duration.as_millis()).ok())
        .unwrap_or_default()
}

#[cfg(test)]
mod tests;
