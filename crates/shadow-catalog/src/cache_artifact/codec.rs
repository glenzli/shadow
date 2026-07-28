//! Persisted enum, digest, and numeric codecs shared by Catalog row owners.

use rusqlite::types::Type;
use shadow_domain::{PreviewByteOrder, PreviewCodec};

use crate::CatalogError;

use super::CachedArtifactRole;

pub(crate) fn parse_role(value: String) -> Result<CachedArtifactRole, CatalogError> {
    match value.as_str() {
        "recipe_preview" => Ok(CachedArtifactRole::RecipePreview),
        "embedded_preview" => Ok(CachedArtifactRole::EmbeddedPreview),
        "generated_proxy" => Ok(CachedArtifactRole::GeneratedProxy),
        _ => Err(unknown("role", value)),
    }
}

pub(crate) fn parse_codec(value: String) -> Result<PreviewCodec, CatalogError> {
    match value.as_str() {
        "unknown" => Ok(PreviewCodec::Unknown),
        "jpeg" => Ok(PreviewCodec::Jpeg),
        "bitmap" => Ok(PreviewCodec::Bitmap),
        "jpeg_xl" => Ok(PreviewCodec::JpegXl),
        "h265" => Ok(PreviewCodec::H265),
        _ => Err(unknown("codec", value)),
    }
}

pub(crate) fn parse_byte_order(value: String) -> Result<PreviewByteOrder, CatalogError> {
    match value.as_str() {
        "not_applicable" => Ok(PreviewByteOrder::NotApplicable),
        "native" => Ok(PreviewByteOrder::Native),
        "little_endian" => Ok(PreviewByteOrder::LittleEndian),
        "big_endian" => Ok(PreviewByteOrder::BigEndian),
        _ => Err(unknown("byte_order", value)),
    }
}

pub(super) fn sqlite_u64(value: u64, field: &'static str) -> Result<i64, CatalogError> {
    i64::try_from(value).map_err(|_| CatalogError::CachedArtifactValueOutOfRange { field })
}

pub(super) fn sqlite_usize(value: usize, field: &'static str) -> Result<i64, CatalogError> {
    i64::try_from(value).map_err(|_| CatalogError::CachedArtifactValueOutOfRange { field })
}

pub(crate) fn non_negative_u64(value: i64, index: usize) -> rusqlite::Result<u64> {
    u64::try_from(value).map_err(|error| conversion(index, error))
}

pub(crate) fn non_negative_u32(value: i64, index: usize) -> rusqlite::Result<u32> {
    u32::try_from(value).map_err(|error| conversion(index, error))
}

pub(crate) fn non_negative_u16(value: i64, index: usize) -> rusqlite::Result<u16> {
    u16::try_from(value).map_err(|error| conversion(index, error))
}

pub(crate) fn optional_usize(value: Option<i64>, index: usize) -> rusqlite::Result<Option<usize>> {
    value
        .map(|value| usize::try_from(value).map_err(|error| conversion(index, error)))
        .transpose()
}

pub(crate) fn digest(value: Vec<u8>, index: usize) -> rusqlite::Result<[u8; 32]> {
    value.try_into().map_err(|value: Vec<u8>| {
        rusqlite::Error::FromSqlConversionFailure(
            index,
            Type::Blob,
            Box::new(std::io::Error::new(
                std::io::ErrorKind::InvalidData,
                format!("expected 32 digest bytes, found {}", value.len()),
            )),
        )
    })
}

fn unknown(field: &'static str, value: String) -> CatalogError {
    CatalogError::UnknownCachedArtifactValue { field, value }
}

fn conversion(
    index: usize,
    error: impl std::error::Error + Send + Sync + 'static,
) -> rusqlite::Error {
    rusqlite::Error::FromSqlConversionFailure(index, Type::Integer, Box::new(error))
}
