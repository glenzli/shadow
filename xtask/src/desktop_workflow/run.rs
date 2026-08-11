use std::{ffi::OsString, io};

pub(super) fn run(arguments: impl IntoIterator<Item = OsString>) -> io::Result<()> {
    #[cfg(target_os = "macos")]
    {
        macos::run(arguments)
    }
    #[cfg(not(target_os = "macos"))]
    {
        let _ = arguments;
        Err(io::Error::new(
            io::ErrorKind::Unsupported,
            "canonical debug launch currently has only a macOS backend",
        ))
    }
}

#[cfg(target_os = "macos")]
pub(super) fn check() -> io::Result<()> {
    macos::run([OsString::from("--check")])
}

#[cfg(target_os = "macos")]
mod macos {
    use std::{
        env,
        ffi::OsString,
        fs, io,
        path::{Path, PathBuf},
        process::{Command, Stdio},
    };

    use super::super::{
        layout::{AppBundlePaths, WorkflowPaths},
        process,
        provider::{self, ModelPaths},
    };

    pub(super) fn run(arguments: impl IntoIterator<Item = OsString>) -> io::Result<()> {
        let mut arguments = arguments.into_iter().collect::<Vec<_>>();
        if arguments
            .first()
            .is_some_and(|argument| argument == "--help" || argument == "-h")
        {
            print_usage();
            return Ok(());
        }
        let paths = WorkflowPaths::resolve()?;
        let bundle = AppBundlePaths::macos(paths.current_app.clone());
        let provider_path = env::var_os("SHADOW_RAWNIND_PROVIDER_PATH")
            .map_or_else(|| bundle.rawnind_provider.clone(), PathBuf::from);
        let provider_root = provider_path
            .parent()
            .ok_or_else(|| io::Error::other("RawNIND provider path has no parent"))?;
        let provider_runtime = provider_root.join("_rawnind_runtime");
        let provider_manifest = env::var_os("SHADOW_RAWNIND_MANIFEST_PATH")
            .map_or_else(|| bundle.rawnind_manifest.clone(), PathBuf::from);
        let models = ModelPaths::resolve()?;
        verify_launch_inputs(
            &bundle,
            &provider_path,
            &provider_runtime,
            &provider_manifest,
        )?;
        provider::verify(&provider_path, &provider_manifest, &models)?;

        if arguments
            .first()
            .is_some_and(|argument| argument == "--check")
        {
            if arguments.len() != 1 {
                return Err(invalid("--check does not accept extra arguments"));
            }
            println!("canonical debug app: {}", bundle.app.display());
            println!("offline city index: {}", bundle.geonames_index.display());
            println!("AI RAW Denoise provider: {}", provider_path.display());
            println!("AI RAW Denoise model package: {}", models.package.display());
            println!("AI RAW Denoise model graph: {}", models.graph.display());
            return Ok(());
        }
        let foreground = arguments
            .first()
            .is_some_and(|argument| argument == "--foreground");
        if foreground {
            arguments.remove(0);
            return process::run(
                Command::new(&bundle.shadow_executable).args(arguments),
                "run Shadow in the foreground",
            );
        }
        let debug_log_root = env::var_os("SHADOW_DEBUG_LOG_ROOT")
            .map_or_else(|| paths.local_build_root.join("logs"), PathBuf::from);
        fs::create_dir_all(&debug_log_root)?;
        let debug_log = debug_log_root.join("shadow-debug.log");
        let (stdout, stderr) = process::append_log(&debug_log)?;
        let child = Command::new(&bundle.shadow_executable)
            .args(arguments)
            .stdin(Stdio::null())
            .stdout(Stdio::from(stdout))
            .stderr(Stdio::from(stderr))
            .spawn()?;
        println!("Shadow started in the background (pid {}).", child.id());
        println!("log: {}", debug_log.display());
        Ok(())
    }

    fn verify_launch_inputs(
        bundle: &AppBundlePaths,
        provider_path: &Path,
        provider_runtime: &Path,
        provider_manifest: &Path,
    ) -> io::Result<()> {
        for (label, path) in [
            ("Shadow executable", bundle.shadow_executable.as_path()),
            ("RAW decode helper", bundle.decode_helper.as_path()),
            ("AI RAW Denoise provider", provider_path),
        ] {
            if !process::is_executable(path) {
                return Err(not_found(format!("{label} is missing: {}", path.display())));
            }
        }
        for (label, path) in [
            ("GeoNames city index", bundle.geonames_index.as_path()),
            ("GeoNames notice", bundle.geonames_notice.as_path()),
            ("AI RAW Denoise manifest", provider_manifest),
            (
                "AI RAW Denoise runtime base library",
                &provider_runtime.join("base_library.zip"),
            ),
        ] {
            if !path.is_file() {
                return Err(not_found(format!("{label} is missing: {}", path.display())));
            }
        }
        Ok(())
    }

    fn invalid(message: &str) -> io::Error {
        io::Error::new(io::ErrorKind::InvalidInput, message)
    }

    fn not_found(message: String) -> io::Error {
        io::Error::new(io::ErrorKind::NotFound, message)
    }

    fn print_usage() {
        println!(
            "usage:\n  cargo xtask desktop-run-debug [Shadow options]\n  \
             cargo xtask desktop-run-debug --foreground [Shadow options]\n  \
             cargo xtask desktop-run-debug --check"
        );
    }
}
