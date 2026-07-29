use std::convert::Infallible;

use crate::{
    ArtifactReference, InputRole, PrivacyClass, RasterExtent, RemoteUploadPreparation,
    RemoteUploadScope, RemoteUploadStore,
    remote::{PreparedRemoteUploadCommit, PreparedRemoteUploadError, prepare_remote_upload},
};

struct Store {
    changed_extent: bool,
}

impl RemoteUploadStore for Store {
    type Error = Infallible;

    fn prepare(
        &mut self,
        request: RemoteUploadPreparation<'_>,
    ) -> Result<PreparedRemoteUploadCommit, Self::Error> {
        Ok(PreparedRemoteUploadCommit {
            store_object_id: "outbound/one".into(),
            storage_revision: 2,
            sanitization_revision: "sanitize-v2".into(),
            outbound_content_hash: "b".repeat(64),
            outbound_byte_len: 512,
            outbound_media_type: "image/png".into(),
            outbound_raster_extent: if self.changed_extent {
                Some(RasterExtent::new(2, 2).expect("changed extent"))
            } else {
                request.raster_extent()
            },
        })
    }
}

fn source() -> ArtifactReference {
    ArtifactReference {
        role: InputRole::CurrentRenderedCrop,
        content_hash: "a".repeat(64),
        byte_len: 4096,
        media_type: "image/png".into(),
        privacy: PrivacyClass::Personal,
    }
}

#[test]
fn store_commit_is_wrapped_with_source_and_sanitized_identity() {
    let source = source();
    let extent = RasterExtent::new(1024, 768).expect("extent");
    let prepared = prepare_remote_upload(
        &mut Store {
            changed_extent: false,
        },
        3,
        &source,
        RemoteUploadScope::BoundedRenderedCrop,
        Some(extent),
    )
    .expect("prepared");

    assert_eq!(prepared.input_index(), 3);
    assert_eq!(prepared.source(), &source);
    assert_eq!(prepared.scope(), RemoteUploadScope::BoundedRenderedCrop);
    assert_eq!(prepared.raster_extent(), Some(extent));
    assert_eq!(prepared.outbound_byte_len(), 512);
    assert_eq!(prepared.outbound_content_hash(), "b".repeat(64));
    assert_eq!(prepared.sanitization_revision(), "sanitize-v2");
}

#[test]
fn store_cannot_change_the_authorized_extent() {
    let source = source();
    let error = prepare_remote_upload(
        &mut Store {
            changed_extent: true,
        },
        0,
        &source,
        RemoteUploadScope::BoundedRenderedCrop,
        Some(RasterExtent::new(1024, 768).expect("extent")),
    )
    .expect_err("extent substitution");
    assert!(matches!(
        error,
        crate::RemoteUploadPreparationFailure::Contract(
            PreparedRemoteUploadError::OutboundRasterExtentMismatch
        )
    ));
}
