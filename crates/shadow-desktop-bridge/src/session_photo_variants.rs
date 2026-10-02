//! Photo Variant lifecycle and desktop projection.

use anyhow::{Context, Result as AnyResult, anyhow};
use shadow_catalog::{
    ActivatePhotoVariant, CreatePhotoVariant, PhotoVariantRecord, RemovePhotoVariant,
    RenamePhotoVariant,
};
use shadow_domain::{EntityId, PhotoId, PhotoVariantId};

use super::{DesktopSession, ffi, wall_clock::current_time_ms};

impl DesktopSession {
    pub(crate) fn create_photo_variant(
        &self,
        photo_id: &str,
        source_path: &str,
        name: &str,
    ) -> AnyResult<ffi::FfiPhotoEditState> {
        let (photo_id, source) = self.validated_photo_source(photo_id, source_path)?;
        let variants = self.catalog.photo_variants(photo_id)?;
        let active = variants
            .iter()
            .find(|variant| variant.is_active)
            .ok_or_else(|| anyhow!("photo has no active Variant"))?;
        self.catalog.create_photo_variant(&CreatePhotoVariant {
            id: PhotoVariantId::new_v7(),
            photo_id,
            name: name.to_owned(),
            source_commit_id: active.head_commit_id,
            activate: true,
            created_at_ms: current_time_ms()?,
        })?;
        self.photo_edit_state_for(photo_id, &source.location.display_path)
    }

    pub(crate) fn rename_photo_variant(
        &self,
        photo_id: &str,
        source_path: &str,
        variant_id: &str,
        name: &str,
    ) -> AnyResult<ffi::FfiPhotoEditState> {
        let (photo_id, source) = self.validated_photo_source(photo_id, source_path)?;
        self.catalog.rename_photo_variant(&RenamePhotoVariant {
            id: parse_variant_id(variant_id)?,
            photo_id,
            name: name.to_owned(),
            updated_at_ms: current_time_ms()?,
        })?;
        self.photo_edit_state_for(photo_id, &source.location.display_path)
    }

    pub(crate) fn activate_photo_variant(
        &self,
        photo_id: &str,
        source_path: &str,
        variant_id: &str,
    ) -> AnyResult<ffi::FfiPhotoEditState> {
        let (photo_id, source) = self.validated_photo_source(photo_id, source_path)?;
        self.catalog.activate_photo_variant(&ActivatePhotoVariant {
            id: parse_variant_id(variant_id)?,
            photo_id,
            updated_at_ms: current_time_ms()?,
        })?;
        self.photo_edit_state_for(photo_id, &source.location.display_path)
    }

    pub(crate) fn remove_photo_variant(
        &self,
        photo_id: &str,
        source_path: &str,
        variant_id: &str,
    ) -> AnyResult<ffi::FfiPhotoEditState> {
        let (photo_id, source) = self.validated_photo_source(photo_id, source_path)?;
        self.catalog.remove_photo_variant(&RemovePhotoVariant {
            id: parse_variant_id(variant_id)?,
            photo_id,
        })?;
        self.photo_edit_state_for(photo_id, &source.location.display_path)
    }

    pub(crate) fn active_photo_variant_id(&self, photo_id: PhotoId) -> AnyResult<PhotoVariantId> {
        self.catalog
            .photo_variants(photo_id)?
            .into_iter()
            .find(|variant| variant.is_active)
            .map(|variant| variant.id)
            .ok_or_else(|| anyhow!("photo has no active Variant"))
    }

    /// Preflight before consuming a proposal; publication still repeats the
    /// Variant expectation inside the existing Recipe transaction.
    pub(crate) fn require_active_photo_variant(
        &self,
        photo_id: PhotoId,
        expected_variant_id: &str,
    ) -> AnyResult<()> {
        let expected = parse_variant_id(expected_variant_id)?;
        let actual = self.active_photo_variant_id(photo_id)?;
        if expected != actual {
            return Err(
                shadow_catalog::CatalogError::PhotoVariantExpectationMismatch {
                    photo_id,
                    expected,
                    actual,
                }
                .into(),
            );
        }
        Ok(())
    }

    pub(crate) fn ffi_photo_variants(
        &self,
        photo_id: PhotoId,
    ) -> AnyResult<(String, Vec<ffi::FfiPhotoVariant>)> {
        let records = self.catalog.photo_variants(photo_id)?;
        let active = records
            .iter()
            .find(|variant| variant.is_active)
            .ok_or_else(|| anyhow!("photo has no active Variant"))?;
        Ok((
            active.id.to_string(),
            records.into_iter().map(ffi_variant).collect(),
        ))
    }
}

fn ffi_variant(record: PhotoVariantRecord) -> ffi::FfiPhotoVariant {
    ffi::FfiPhotoVariant {
        variant_id: record.id.to_string(),
        name: record.name,
        has_head: record.head_commit_id.is_some(),
        head_commit_id: record
            .head_commit_id
            .map_or_else(String::new, |id| id.to_string()),
        is_default: record.is_default,
        is_active: record.is_active,
        created_at_ms: record.created_at_ms,
        updated_at_ms: record.updated_at_ms,
    }
}

fn parse_variant_id(value: &str) -> AnyResult<PhotoVariantId> {
    value
        .parse()
        .with_context(|| format!("parse photo Variant id {value}"))
}
