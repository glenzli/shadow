use std::{
    env, io,
    path::{Path, PathBuf},
    process::Command,
};

use super::process;

const REQUIRED_OPTIONS: &[&str] = &[
    "--model-package",
    "--model-graph",
    "--manifest",
    "--input-raw",
    "--input-raw-frame",
    "--output-foundation",
    "--source-pixel-contract-sha256",
    "--verify-model",
    "--plan",
    "--run",
];

#[derive(Debug, Clone)]
pub(super) struct ModelPaths {
    pub package: PathBuf,
    pub graph: PathBuf,
}

impl ModelPaths {
    pub(super) fn resolve() -> io::Result<Self> {
        let user_profile_root = user_profile_root()?;
        #[cfg(target_os = "macos")]
        let default_root = user_profile_root.join(
            "Library/Application Support/Shadow/Shadow/models/rawnind-public-bayer-release-5.6.0",
        );
        #[cfg(target_os = "windows")]
        let default_root = user_profile_root
            .join("AppData/Roaming/Shadow/Shadow/models/rawnind-public-bayer-release-5.6.0");
        #[cfg(not(any(target_os = "macos", target_os = "windows")))]
        let default_root =
            user_profile_root.join(".local/share/Shadow/models/rawnind-public-bayer-release-5.6.0");
        Ok(Self {
            package: env::var_os("SHADOW_RAWNIND_PACKAGE_PATH").map_or_else(
                || default_root.join("rawdenoise-nind.dtmodel"),
                PathBuf::from,
            ),
            graph: env::var_os("SHADOW_RAWNIND_BAYER_GRAPH_PATH")
                .map_or_else(|| default_root.join("model_bayer.onnx"), PathBuf::from),
        })
    }
}

pub(super) fn verify(provider: &Path, manifest: &Path, models: &ModelPaths) -> io::Result<()> {
    if !process::is_executable(provider) {
        return Err(not_found(format!(
            "AI RAW Denoise provider is missing: {}",
            provider.display()
        )));
    }
    if !manifest.is_file() || !models.package.is_file() || !models.graph.is_file() {
        return Err(not_found(format!(
            "AI RAW Denoise model inputs are incomplete: manifest={} package={} graph={}",
            manifest.display(),
            models.package.display(),
            models.graph.display()
        )));
    }
    let help = process::capture(
        Command::new(provider).arg("--help"),
        "inspect AI RAW Denoise provider command surface",
    )?;
    for required in REQUIRED_OPTIONS {
        if !help.contains(required) {
            return Err(io::Error::other(format!(
                "AI RAW Denoise provider is incompatible: missing {required}"
            )));
        }
    }
    process::run(
        Command::new(provider)
            .arg("--model-package")
            .arg(&models.package)
            .arg("--model-graph")
            .arg(&models.graph)
            .arg("--manifest")
            .arg(manifest)
            .arg("--verify-model"),
        "verify AI RAW Denoise provider and model",
    )
}

fn user_profile_root() -> io::Result<PathBuf> {
    let variable = if cfg!(target_os = "windows") {
        "USERPROFILE"
    } else {
        "HOME"
    };
    env::var_os(variable)
        .filter(|value| !value.is_empty())
        .map(PathBuf::from)
        .ok_or_else(|| not_found(format!("{variable} is empty; cannot resolve local models")))
}

fn not_found(message: String) -> io::Error {
    io::Error::new(io::ErrorKind::NotFound, message)
}
