use std::{fs, net::SocketAddr, path::PathBuf, sync::Arc};

use shadow_cache::ContentAddressedStore;
use shadow_domain::{EntityId, ImageDimensions, PhotoId, PreviewCodec, RepresentationId};

use crate::{
    AuthorizationToken, LibraryClient, LibraryClientConfig, LibraryServer, LibraryServerConfig,
    LibraryShareSource, OriginalMaterializer, OriginalMaterializerPolicy, RemoteLibraryMirror,
    protocol::{
        CapabilityAvailability, LIBRARY_PROTOCOL_VERSION, MAX_LIBRARY_PAGE_SIZE,
        MAX_ORIGINAL_CHUNK_BYTES, OriginalChunk, PreparedOriginal, RemoteError, RemoteErrorCode,
        RemotePhotoManifest, RemotePhotoMetadata, RemotePhotoPage, RemotePreviewAvailability,
        RemotePreviewManifest, RemotePreviewRole, ServerCapabilities, ServerId, ServerInfo,
    },
    server::remote_error,
};

#[test]
fn authenticated_manifest_proxy_and_original_round_trip() {
    let fixture = FixtureSource::new();
    let original = fixture.original.clone();
    let photo_id = fixture.photo_id;
    let representation_id = fixture.representation_id;
    let token = AuthorizationToken::parse("01234567890123456789012345678901").expect("token");
    let address: SocketAddr = "127.0.0.1:0".parse().expect("bind address");
    let running = LibraryServer::new(
        LibraryServerConfig::new(address, token.clone()),
        Arc::new(fixture),
    )
    .start()
    .expect("start server");
    let client = LibraryClient::new(LibraryClientConfig::new(running.local_address(), token));

    let root = temporary_directory("round-trip");
    let preview_store = ContentAddressedStore::open(root.join("previews")).expect("preview store");
    let mut mirror = RemoteLibraryMirror::open(root.join("mirror")).expect("mirror");
    let report = mirror.sync(&client, &preview_store).expect("sync mirror");
    assert_eq!(report.photo_count, 1);
    assert_eq!(report.downloaded_previews, 1);
    assert!(mirror.snapshot().photos[0].cached_preview.is_some());

    let materializer = OriginalMaterializer::open(
        root.join("originals"),
        OriginalMaterializerPolicy {
            chunk_bytes: 7,
            maximum_original_bytes: 1_024,
        },
    )
    .expect("materializer");
    let first = materializer
        .materialize(&client, photo_id, representation_id)
        .expect("materialize original");
    assert!(!first.reused_existing);
    assert_eq!(
        fs::read(&first.path).expect("read materialized bytes"),
        original
    );
    let second = materializer
        .materialize(&client, photo_id, representation_id)
        .expect("reuse original");
    assert!(second.reused_existing);
    assert_eq!(second.path, first.path);

    running.shutdown().expect("stop server");
    fs::remove_dir_all(root).expect("remove fixture");
}

#[derive(Debug)]
struct FixtureSource {
    server_id: ServerId,
    photo_id: PhotoId,
    representation_id: RepresentationId,
    preview: Vec<u8>,
    original: Vec<u8>,
}

impl FixtureSource {
    fn new() -> Self {
        Self {
            server_id: ServerId(uuid::Uuid::now_v7()),
            photo_id: PhotoId::new_v7(),
            representation_id: RepresentationId::new_v7(),
            preview: b"fixture-preview-jpeg".to_vec(),
            original: b"fixture-raw-original-payload".to_vec(),
        }
    }

    fn preview_manifest(&self) -> RemotePreviewManifest {
        RemotePreviewManifest {
            role: RemotePreviewRole::EmbeddedPreview,
            digest_blake3: *blake3::hash(&self.preview).as_bytes(),
            byte_len: u64::try_from(self.preview.len()).unwrap_or(u64::MAX),
            codec: PreviewCodec::Jpeg,
            dimensions: ImageDimensions {
                width: 640,
                height: 480,
            },
        }
    }

    fn original_manifest(&self) -> PreparedOriginal {
        PreparedOriginal {
            server_id: self.server_id,
            photo_id: self.photo_id,
            representation_id: self.representation_id,
            revision_token: "fixture-revision".to_owned(),
            digest_blake3: *blake3::hash(&self.original).as_bytes(),
            byte_len: u64::try_from(self.original.len()).unwrap_or(u64::MAX),
            display_name: "fixture.nef".to_owned(),
        }
    }
}

impl LibraryShareSource for FixtureSource {
    fn server_info(&self) -> ServerInfo {
        ServerInfo {
            protocol_version: LIBRARY_PROTOCOL_VERSION,
            server_id: self.server_id,
            display_name: "Fixture Mac".to_owned(),
            capabilities: ServerCapabilities {
                serves_embedded_previews: CapabilityAvailability::Available,
                serves_generated_proxies: CapabilityAvailability::Available,
                serves_originals: CapabilityAvailability::Available,
                private_preview_provider: CapabilityAvailability::Unavailable,
                maximum_page_size: MAX_LIBRARY_PAGE_SIZE,
                maximum_original_chunk_bytes: MAX_ORIGINAL_CHUNK_BYTES,
            },
        }
    }

    fn list_photos(
        &self,
        cursor: Option<&str>,
        _limit: u16,
    ) -> Result<RemotePhotoPage, RemoteError> {
        if cursor.is_some() {
            return Err(remote_error(
                RemoteErrorCode::InvalidRequest,
                "fixture has one page",
            ));
        }
        Ok(RemotePhotoPage {
            server_id: self.server_id,
            items: vec![RemotePhotoManifest {
                photo_id: self.photo_id,
                representation_id: self.representation_id,
                display_name: "fixture.nef".to_owned(),
                source_byte_len: u64::try_from(self.original.len()).unwrap_or(u64::MAX),
                source_modified_at_ms: Some(17),
                metadata: RemotePhotoMetadata {
                    camera_make: "Fixture".to_owned(),
                    camera_model: "Camera".to_owned(),
                    ..RemotePhotoMetadata::default()
                },
                preview: RemotePreviewAvailability::Available(self.preview_manifest()),
            }],
            next_cursor: None,
        })
    }

    fn fetch_preview(
        &self,
        digest_blake3: [u8; 32],
    ) -> Result<(RemotePreviewManifest, Vec<u8>), RemoteError> {
        let manifest = self.preview_manifest();
        if manifest.digest_blake3 != digest_blake3 {
            return Err(remote_error(RemoteErrorCode::NotFound, "preview not found"));
        }
        Ok((manifest, self.preview.clone()))
    }

    fn prepare_original(
        &self,
        photo_id: PhotoId,
        representation_id: RepresentationId,
    ) -> Result<PreparedOriginal, RemoteError> {
        if photo_id != self.photo_id || representation_id != self.representation_id {
            return Err(remote_error(
                RemoteErrorCode::NotFound,
                "original not found",
            ));
        }
        Ok(self.original_manifest())
    }

    fn read_original(
        &self,
        revision_token: &str,
        offset: u64,
        maximum_bytes: u32,
    ) -> Result<(OriginalChunk, Vec<u8>), RemoteError> {
        if revision_token != "fixture-revision" {
            return Err(remote_error(
                RemoteErrorCode::NotFound,
                "revision not found",
            ));
        }
        let start = usize::try_from(offset)
            .map_err(|_| remote_error(RemoteErrorCode::InvalidRequest, "offset is too large"))?;
        if start > self.original.len() {
            return Err(remote_error(
                RemoteErrorCode::InvalidRequest,
                "offset is invalid",
            ));
        }
        let end = start
            .saturating_add(usize::try_from(maximum_bytes).unwrap_or(usize::MAX))
            .min(self.original.len());
        Ok((
            OriginalChunk {
                offset,
                complete: end == self.original.len(),
            },
            self.original[start..end].to_vec(),
        ))
    }
}

fn temporary_directory(label: &str) -> PathBuf {
    let path = std::env::temp_dir().join(format!(
        "shadow-library-sharing-{label}-{}",
        uuid::Uuid::now_v7()
    ));
    fs::create_dir_all(&path).expect("create fixture directory");
    path
}
