use std::{ffi::OsString, io, path::Path};

pub(super) fn run(arguments: impl IntoIterator<Item = OsString>) -> io::Result<()> {
    let values = arguments.into_iter().collect::<Vec<_>>();
    if values.len() != 2 {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "usage: cargo xtask desktop-promote-debug /absolute/path/to/Shadow.app validation-label",
        ));
    }
    let candidate = Path::new(&values[0]);
    let validation_label = values[1].to_string_lossy();
    #[cfg(target_os = "macos")]
    {
        promote_candidate(candidate, &validation_label)
    }
    #[cfg(not(target_os = "macos"))]
    {
        let _ = (candidate, validation_label);
        Err(io::Error::new(
            io::ErrorKind::Unsupported,
            "canonical debug promotion currently has only a macOS backend",
        ))
    }
}

#[cfg(target_os = "macos")]
pub(super) fn promote_candidate(candidate: &Path, validation_label: &str) -> io::Result<()> {
    macos::promote_candidate(candidate, validation_label)
}

#[cfg(target_os = "macos")]
mod macos {
    use std::{
        fs,
        io::{self, Write},
        os::unix::fs::symlink,
        path::{Path, PathBuf},
        process::Command,
    };

    use super::super::{
        acquire_canonical_debug_lock,
        layout::{AppBundlePaths, WorkflowPaths},
        process,
    };

    pub(super) fn promote_candidate(candidate: &Path, validation_label: &str) -> io::Result<()> {
        validate_request(candidate, validation_label)?;
        let paths = WorkflowPaths::resolve()?;
        let candidate_paths = AppBundlePaths::macos(candidate.to_path_buf());
        verify_bundle(&candidate_paths)?;
        lint_bundle_plists(&candidate_paths)?;

        fs::create_dir_all(&paths.local_build_root).map_err(|error| {
            io::Error::new(
                error.kind(),
                format!(
                    "cannot create local build root {}: {error}",
                    paths.local_build_root.display()
                ),
            )
        })?;
        let release_root = paths.local_build_root.join("releases/debug");
        fs::create_dir_all(&release_root)?;
        let current_link = paths.local_build_root.join("current-debug");
        ensure_symlink_or_absent(&current_link)?;

        let _lock = acquire_canonical_debug_lock()?;
        let mut cleanup = PromotionCleanup::new();

        let revision = process::capture(
            Command::new("git")
                .current_dir(&paths.repository_root)
                .args(["rev-parse", "--short=12", "HEAD"]),
            "resolve promotion revision",
        )?;
        let promoted_at = process::capture(
            Command::new("date").args(["-u", "+%Y%m%dT%H%M%SZ"]),
            "timestamp",
        )?;
        let release_id = format!("{revision}-{promoted_at}");
        let release_directory = release_root.join(&release_id);
        if release_directory.exists() {
            return Err(io::Error::new(
                io::ErrorKind::AlreadyExists,
                format!("release already exists: {}", release_directory.display()),
            ));
        }
        let incoming = release_root.join(format!(".incoming-{release_id}-{}", std::process::id()));
        fs::create_dir(&incoming)?;
        cleanup.incoming = Some(incoming.clone());
        let copied_app = incoming.join("Shadow.app");
        process::run(
            Command::new("ditto").arg(candidate).arg(&copied_app),
            "copy candidate application",
        )?;
        let copied_paths = AppBundlePaths::macos(copied_app);
        let digests = verify_copy_digests(&candidate_paths, &copied_paths)?;
        let worktree_status = git_status(&paths.repository_root)?;
        let worktree_digest = process::sha256_bytes(&worktree_status)?;
        let manifest = Manifest {
            revision: &revision,
            promoted_at: &promoted_at,
            validation_label,
            source_app: candidate,
            worktree_digest: &worktree_digest,
            digests: &digests,
        };
        write_manifest(&incoming.join("build-manifest.txt"), &manifest)?;
        fs::rename(&incoming, &release_directory)?;
        cleanup.incoming = None;

        let next_link = paths.local_build_root.join(format!(
            ".current-debug-{release_id}-{}",
            std::process::id()
        ));
        symlink(Path::new("releases/debug").join(&release_id), &next_link)?;
        cleanup.next_link = Some(next_link.clone());
        fs::rename(&next_link, &current_link)?;
        cleanup.next_link = None;

        let canonical = AppBundlePaths::macos(current_link.join("Shadow.app"));
        verify_bundle(&canonical)?;
        println!("canonical debug app: {}", canonical.app.display());
        println!(
            "run: cargo xtask desktop-run-debug (or {})",
            paths.repository_root.join("scripts/run_debug.sh").display()
        );
        Ok(())
    }

    #[derive(Debug)]
    struct BundleDigests {
        shadow: String,
        decode_helper: String,
        composition_worker: String,
        server: String,
        server_decode_helper: String,
        geonames_index: String,
        geonames_notice: String,
    }

    struct Manifest<'a> {
        revision: &'a str,
        promoted_at: &'a str,
        validation_label: &'a str,
        source_app: &'a Path,
        worktree_digest: &'a str,
        digests: &'a BundleDigests,
    }

    fn verify_bundle(bundle: &AppBundlePaths) -> io::Result<()> {
        for (label, path) in [
            ("Shadow executable", &bundle.shadow_executable),
            ("isolated RAW decode helper", &bundle.decode_helper),
            ("photo composition worker", &bundle.composition_worker),
            ("Shadow Server executable", &bundle.server_executable),
            ("Shadow Server decode helper", &bundle.server_decode_helper),
        ] {
            if !process::is_executable(path) {
                return Err(not_found(format!("{label} is missing: {}", path.display())));
            }
        }
        for (label, path) in [
            ("GeoNames city index", &bundle.geonames_index),
            ("GeoNames notice", &bundle.geonames_notice),
        ] {
            if !path.is_file() {
                return Err(not_found(format!("{label} is missing: {}", path.display())));
            }
        }
        Ok(())
    }

    fn verify_copy_digests(
        source: &AppBundlePaths,
        copied: &AppBundlePaths,
    ) -> io::Result<BundleDigests> {
        let pairs = [
            (&source.shadow_executable, &copied.shadow_executable),
            (&source.decode_helper, &copied.decode_helper),
            (&source.composition_worker, &copied.composition_worker),
            (&source.server_executable, &copied.server_executable),
            (&source.server_decode_helper, &copied.server_decode_helper),
            (&source.geonames_index, &copied.geonames_index),
            (&source.geonames_notice, &copied.geonames_notice),
        ];
        let mut verified = Vec::with_capacity(pairs.len());
        for (source_path, copied_path) in pairs {
            let source_digest = process::sha256_file(source_path)?;
            let copied_digest = process::sha256_file(copied_path)?;
            if source_digest != copied_digest {
                return Err(io::Error::other(format!(
                    "copied application digest verification failed for {}",
                    source_path.display()
                )));
            }
            verified.push(source_digest);
        }
        let [
            shadow,
            decode_helper,
            composition_worker,
            server,
            server_decode_helper,
            geonames_index,
            geonames_notice,
        ] = verified.try_into().map_err(|_| {
            io::Error::other("internal digest projection did not retain every bundle path")
        })?;
        Ok(BundleDigests {
            shadow,
            decode_helper,
            composition_worker,
            server,
            server_decode_helper,
            geonames_index,
            geonames_notice,
        })
    }

    fn write_manifest(path: &Path, manifest: &Manifest<'_>) -> io::Result<()> {
        let mut file = fs::File::create(path)?;
        writeln!(file, "schema=shadow-canonical-debug-build-v1")?;
        writeln!(file, "revision={}", manifest.revision)?;
        writeln!(file, "promoted_at_utc={}", manifest.promoted_at)?;
        writeln!(file, "validation={}", manifest.validation_label)?;
        writeln!(file, "source_app={}", manifest.source_app.display())?;
        writeln!(file, "worktree_status_sha256={}", manifest.worktree_digest)?;
        writeln!(file, "shadow_executable_sha256={}", manifest.digests.shadow)?;
        writeln!(
            file,
            "decode_helper_sha256={}",
            manifest.digests.decode_helper
        )?;
        writeln!(
            file,
            "composition_worker_sha256={}",
            manifest.digests.composition_worker
        )?;
        writeln!(file, "server_executable_sha256={}", manifest.digests.server)?;
        writeln!(
            file,
            "server_decode_helper_sha256={}",
            manifest.digests.server_decode_helper
        )?;
        writeln!(
            file,
            "geonames_city_index_sha256={}",
            manifest.digests.geonames_index
        )?;
        writeln!(
            file,
            "geonames_notice_sha256={}",
            manifest.digests.geonames_notice
        )?;
        Ok(())
    }

    fn validate_request(candidate: &Path, validation_label: &str) -> io::Result<()> {
        if !candidate.is_absolute() {
            return Err(invalid("candidate app path must be absolute"));
        }
        if validation_label.is_empty()
            || validation_label.contains('\n')
            || validation_label.contains('\r')
        {
            return Err(invalid("validation label must be one non-empty line"));
        }
        Ok(())
    }

    fn ensure_symlink_or_absent(path: &Path) -> io::Result<()> {
        match fs::symlink_metadata(path) {
            Ok(metadata) if metadata.file_type().is_symlink() => Ok(()),
            Ok(_) => Err(io::Error::new(
                io::ErrorKind::AlreadyExists,
                format!(
                    "canonical entry exists but is not a symlink: {}",
                    path.display()
                ),
            )),
            Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(()),
            Err(error) => Err(error),
        }
    }

    fn lint_bundle_plists(bundle: &AppBundlePaths) -> io::Result<()> {
        let app_plist = bundle.app.join("Contents/Info.plist");
        if app_plist.is_file() {
            process::run(
                Command::new("plutil").args(["-lint"]).arg(&app_plist),
                "validate Shadow Info.plist",
            )?;
            process::run(
                Command::new("plutil")
                    .args(["-lint"])
                    .arg(bundle.server_app.join("Contents/Info.plist")),
                "validate Shadow Server Info.plist",
            )?;
        }
        Ok(())
    }

    fn git_status(repository_root: &Path) -> io::Result<Vec<u8>> {
        let output = Command::new("git")
            .current_dir(repository_root)
            .args(["status", "--porcelain=v1", "--untracked-files=all"])
            .output()?;
        if output.status.success() {
            Ok(output.stdout)
        } else {
            Err(io::Error::other(format!(
                "capture worktree status exited with {}: {}",
                output.status,
                String::from_utf8_lossy(&output.stderr).trim()
            )))
        }
    }

    struct PromotionCleanup {
        incoming: Option<PathBuf>,
        next_link: Option<PathBuf>,
    }

    impl PromotionCleanup {
        fn new() -> Self {
            Self {
                incoming: None,
                next_link: None,
            }
        }
    }

    impl Drop for PromotionCleanup {
        fn drop(&mut self) {
            if let Some(next_link) = self.next_link.take() {
                let _ = fs::remove_file(next_link);
            }
            if let Some(incoming) = self.incoming.take() {
                let _ = fs::remove_dir_all(incoming);
            }
        }
    }

    fn invalid(message: &str) -> io::Error {
        io::Error::new(io::ErrorKind::InvalidInput, message)
    }

    fn not_found(message: String) -> io::Error {
        io::Error::new(io::ErrorKind::NotFound, message)
    }
}
