use std::{
    env, io,
    path::{Path, PathBuf},
};

pub(super) const CITY_INDEX_NAME: &str = "shadow-geonames-cities-v1.tsv";

#[derive(Debug, Clone)]
pub(super) struct WorkflowPaths {
    pub repository_root: PathBuf,
    pub local_build_root: PathBuf,
    pub asset_root: PathBuf,
    pub build_directory: PathBuf,
    pub current_app: PathBuf,
}

impl WorkflowPaths {
    pub(super) fn resolve() -> io::Result<Self> {
        let repository_root = repository_root()?;
        let repository_parent = repository_root
            .parent()
            .ok_or_else(|| io::Error::other("repository root has no parent"))?;
        let local_build_root = env_path(
            "SHADOW_LOCAL_BUILD_ROOT",
            repository_parent.join(".shadow-local-build"),
        );
        let asset_root = env_path(
            "SHADOW_CANONICAL_DEBUG_ASSET_ROOT",
            repository_parent.join(".shadow-local-assets/canonical-debug"),
        );
        let build_directory = env_path(
            "SHADOW_CANONICAL_DEBUG_BUILD_DIR",
            local_build_root.join("canonical-debug-build"),
        );
        validate_external_path(
            &repository_root,
            &build_directory,
            "SHADOW_CANONICAL_DEBUG_BUILD_DIR",
        )?;
        let current_app = local_build_root.join("current-debug/Shadow.app");
        Ok(Self {
            repository_root,
            local_build_root,
            asset_root,
            build_directory,
            current_app,
        })
    }
}

#[derive(Debug, Clone)]
pub(super) struct AppBundlePaths {
    pub app: PathBuf,
    pub shadow_executable: PathBuf,
    pub decode_helper: PathBuf,
    pub composition_worker: PathBuf,
    pub server_app: PathBuf,
    pub server_executable: PathBuf,
    pub server_decode_helper: PathBuf,
    pub geonames_index: PathBuf,
    pub geonames_notice: PathBuf,
}

impl AppBundlePaths {
    pub(super) fn macos(app: PathBuf) -> Self {
        let contents = app.join("Contents");
        let macos = contents.join("MacOS");
        let server_app = contents.join("Applications/Shadow Server.app");
        let server_macos = server_app.join("Contents/MacOS");
        let geonames = contents.join("Resources/GeoNames");
        Self {
            app,
            shadow_executable: macos.join("Shadow"),
            decode_helper: macos.join("shadow-image-decode-helper"),
            composition_worker: macos.join("shadow-photo-composition-worker"),
            server_app,
            server_executable: server_macos.join("Shadow Server"),
            server_decode_helper: server_macos.join("shadow-image-decode-helper"),
            geonames_index: geonames.join(CITY_INDEX_NAME),
            geonames_notice: geonames.join("NOTICE.txt"),
        }
    }
}

pub(super) fn repository_root() -> io::Result<PathBuf> {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .ok_or_else(|| io::Error::other("xtask has no repository parent"))?
        .canonicalize()
}

pub(super) fn validate_external_path(
    repository_root: &Path,
    path: &Path,
    variable: &str,
) -> io::Result<()> {
    if !path.is_absolute() || path.starts_with(repository_root) {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            format!(
                "{variable} must be an absolute path outside {}; received {}",
                repository_root.display(),
                path.display()
            ),
        ));
    }
    Ok(())
}

fn env_path(variable: &str, default: PathBuf) -> PathBuf {
    env::var_os(variable).map_or(default, PathBuf::from)
}
