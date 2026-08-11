use serde::{Deserialize, Serialize};

/// The operating system whose native path encoding is stored in a location.
#[derive(Debug, Copy, Clone, Eq, PartialEq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Platform {
    MacOs,
    Windows,
    OtherUnix,
}

impl Platform {
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::MacOs => "macos",
            Self::Windows => "windows",
            Self::OtherUnix => "other_unix",
        }
    }
}

/// A persisted asset representation. This is intentionally distinct from a
/// decoder's output capability such as `SensorMosaic` or `SceneLinearRGB`.
#[derive(Debug, Copy, Clone, Eq, PartialEq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum RepresentationKind {
    OriginalRaw,
    OriginalRaster,
    DerivedDng,
    EmbeddedPreview,
    SceneLinearRgb,
    VendorRenderedRgb,
    Proxy,
}

impl RepresentationKind {
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::OriginalRaw => "original_raw",
            Self::OriginalRaster => "original_raster",
            Self::DerivedDng => "derived_dng",
            Self::EmbeddedPreview => "embedded_preview",
            Self::SceneLinearRgb => "scene_linear_rgb",
            Self::VendorRenderedRgb => "vendor_rendered_rgb",
            Self::Proxy => "proxy",
        }
    }
}

/// Whether a concrete location can currently be used.
#[derive(Debug, Copy, Clone, Eq, PartialEq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum LocationStatus {
    Online,
    Offline,
    NeedsRevalidation,
}

impl LocationStatus {
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Online => "online",
            Self::Offline => "offline",
            Self::NeedsRevalidation => "needs_revalidation",
        }
    }
}

/// A platform-local path. Unix and macOS store the original path bytes in
/// `native_path`; Windows stores the original little-endian UTF-16 code units.
/// `display_path` is only for UI and logs and must never be used as identity or
/// to reopen the filesystem object.
#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct AssetLocation {
    pub platform: Platform,
    pub native_path: Vec<u8>,
    pub display_path: String,
}

impl AssetLocation {
    pub fn new(platform: Platform, native_path: Vec<u8>, display_path: impl Into<String>) -> Self {
        Self {
            platform,
            native_path,
            display_path: display_path.into(),
        }
    }
}
