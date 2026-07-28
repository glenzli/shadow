//! `DesktopSession` facade for exact selected-photo inspection.

use anyhow::Result as AnyResult;

use crate::{DesktopSession, ffi};

impl DesktopSession {
    pub(crate) fn photo_inspection(
        &self,
        photo_id: &str,
        representation_id: &str,
    ) -> AnyResult<ffi::FfiPhotoInspection> {
        self.photo_inspection.inspect(photo_id, representation_id)
    }
}
