//! Windows deployment keeps immutable release directories behind an atomic pointer file.

use std::{env, ffi::OsString, io, path::Path, process::Command};

use super::{layout::WorkflowPaths, lock::CanonicalDebugLock, process};

pub(super) fn build(check_only: bool, label: Option<&str>) -> io::Result<()> {
    let paths = WorkflowPaths::resolve()?;
    let city_index = env::var_os("SHADOW_GEONAMES_CITY_INDEX_PATH")
        .map(std::path::PathBuf::from)
        .unwrap_or_else(|| {
            paths
                .asset_root
                .join("geonames")
                .join(super::layout::CITY_INDEX_NAME)
        });
    if !city_index.is_file() {
        return Err(io::Error::new(
            io::ErrorKind::NotFound,
            "set SHADOW_GEONAMES_CITY_INDEX_PATH to a prepared offline city index",
        ));
    }
    if check_only {
        println!(
            "Windows build directory: {}",
            paths.build_directory.display()
        );
        println!("offline city index: {}", city_index.display());
        return Ok(());
    }
    crate::local_workspace_guard::run([
        OsString::from("--root"),
        paths.repository_root.clone().into_os_string(),
    ])?;
    crate::desktop_i18n::run()?;
    let mut configure = Command::new("cmake");
    configure
        .current_dir(&paths.repository_root)
        .args(["--preset", "windows-desktop-dev", "-B"])
        .arg(&paths.build_directory)
        .arg("-DSHADOW_BUILD_SERVER_MANAGER=ON")
        .arg(format!(
            "-DSHADOW_GEONAMES_CITY_INDEX_PATH={}",
            city_index.display()
        ));
    if let Some(installed) = env::var_os("VCPKG_INSTALLED_DIR") {
        configure
            .arg("-DVCPKG_MANIFEST_MODE=OFF")
            .arg(format!(
                "-DVCPKG_INSTALLED_DIR={}",
                Path::new(&installed).display()
            ))
            .arg(format!(
                "-DSHADOW_VCPKG_ICONV_PREFIX={}",
                Path::new(&installed)
                    .join("x64-windows/usr/local")
                    .display()
            ));
    }
    if let Some(toolkit) = env::var_os("SHADOW_CUDA_TOOLKIT_ROOT") {
        let toolkit = Path::new(&toolkit);
        configure
            .arg("-DSHADOW_ENABLE_CUDA=ON")
            .arg(format!("-DCUDAToolkit_ROOT={}", toolkit.display()))
            .arg(format!(
                "-DCMAKE_CUDA_COMPILER={}",
                toolkit.join("bin/nvcc.exe").display()
            ))
            .arg("-DCMAKE_CUDA_ARCHITECTURES=native");
    }
    if let Some(database) = env::var_os("SHADOW_LENSFUN_DATABASE_PATH") {
        configure.arg(format!(
            "-DSHADOW_LENSFUN_DATABASE_PATH={}",
            Path::new(&database).display()
        ));
    }
    process::run(&mut configure, "configure Windows desktop")?;
    process::run(
        Command::new("cmake")
            .arg("--build")
            .arg(&paths.build_directory)
            .args([
                "--target",
                "shadow-desktop",
                "shadow-server-manager",
                "--parallel",
                "6",
            ]),
        "build Windows applications",
    )?;
    // Windows promotion always verifies both real application entry points.
    process::run(
        Command::new("ctest")
            .arg("--test-dir")
            .arg(&paths.build_directory)
            .args([
                "--output-on-failure",
                "-R",
                "^(shadow-desktop-qml-startup|shadow-server-manager-qml-startup)$",
            ]),
        "verify Windows application startup",
    )?;
    let candidate = paths
        .build_directory
        .join(format!("windows-deployment-{}", std::process::id()));
    process::run(
        Command::new("cmake")
            .arg("--install")
            .arg(&paths.build_directory)
            .arg("--prefix")
            .arg(&candidate),
        "deploy Windows applications",
    )?;
    promote(&candidate, label.unwrap_or("windows-startup-verified"))
}

fn command(paths: &WorkflowPaths, action: &str) -> Command {
    let mut command = Command::new(env::var_os("PYTHON").unwrap_or_else(|| "python".into()));
    command
        .arg(
            paths
                .repository_root
                .join("scripts/desktop_windows_runtime.py"),
        )
        .arg(action)
        .arg("--build-root")
        .arg(&paths.local_build_root);
    command
}

pub(super) fn promote(candidate: &Path, label: &str) -> io::Result<()> {
    let paths = WorkflowPaths::resolve()?;
    std::fs::create_dir_all(&paths.local_build_root)?;
    let _lock = CanonicalDebugLock::acquire(&paths.repository_root, &paths.local_build_root)?;
    process::run(
        command(&paths, "promote")
            .arg("--candidate")
            .arg(candidate)
            .arg("--label")
            .arg(label)
            .arg("--source-root")
            .arg(&paths.repository_root),
        "promote Windows debug applications",
    )
}

pub(super) fn launch(arguments: impl IntoIterator<Item = OsString>) -> io::Result<()> {
    let paths = WorkflowPaths::resolve()?;
    process::run(
        command(&paths, "run").arg("--").args(arguments),
        "launch Windows debug applications",
    )
}
