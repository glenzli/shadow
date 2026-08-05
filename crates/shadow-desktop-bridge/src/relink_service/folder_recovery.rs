//! Safe batch planning for an explicitly selected replacement folder.
//!
//! The selected photo establishes an old-directory to new-directory mapping.
//! Every other active original in that exact old directory is then considered
//! before the ordinary scanner runs. Current strong identities require an
//! exact digest match; older records may use one unique same-name/same-size
//! candidate and gain their first identity during application.

use std::{
    collections::HashMap,
    fs::File,
    io::Read,
    path::{Path, PathBuf},
};

use anyhow::{Context, Result as AnyResult, anyhow, bail};
use shadow_catalog::{
    CatalogHandle, ContentIdentity, LibrarySourceRecord, MissingSourceLocationRecord,
};
use shadow_core::{native_location, native_path_from_location};
use shadow_domain::RepresentationId;

#[derive(Debug, Clone, Eq, PartialEq)]
pub(super) struct PlannedRecovery {
    pub missing: MissingSourceLocationRecord,
    pub path: PathBuf,
    pub may_bootstrap_identity: bool,
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub(super) struct PlannedSourceRecovery {
    pub recoveries: Vec<PlannedRecovery>,
    pub missing_target_count: usize,
    pub source_root_unavailable: bool,
}

#[derive(Debug, Clone, Eq, PartialEq)]
struct FileObservation {
    path: PathBuf,
    byte_len: u64,
}

pub(super) fn plan_folder_recovery(
    catalog: &CatalogHandle,
    selected_missing: &MissingSourceLocationRecord,
    selected_root: &Path,
) -> AnyResult<Vec<PlannedRecovery>> {
    let selected_candidate = find_selected_candidate(catalog, selected_missing, selected_root)?;
    let replacement_directory = selected_candidate
        .path
        .parent()
        .map(Path::to_path_buf)
        .ok_or_else(|| anyhow!("the selected original has no containing folder"))?;
    let original_path = native_path_from_location(&selected_missing.location)
        .context("decode the historical original path")?;
    let original_directory = original_path
        .parent()
        .map(Path::to_path_buf)
        .ok_or_else(|| anyhow!("the historical original has no containing folder"))?;
    let historical_root = native_location(&original_directory);
    let mut targets = catalog
        .library_source_relink_targets_beneath(&historical_root)?
        .into_iter()
        .filter(|target| has_exact_parent(target, &original_directory))
        .filter(|target| !historical_source_is_available(target))
        .collect::<Vec<_>>();
    if !targets
        .iter()
        .any(|target| target.location_id == selected_missing.location_id)
    {
        targets.push(selected_missing.clone());
    }
    targets.sort_by_key(|target| {
        (
            target.location_id != selected_missing.location_id,
            target.location.native_path.clone(),
            target.location_id,
        )
    });

    let files = direct_file_inventory(&replacement_directory)?;
    let mut identity_cache = HashMap::new();
    let mut claimed_paths = HashMap::<PathBuf, RepresentationId>::new();
    let mut plan = Vec::new();
    for target in targets {
        let required = target.location_id == selected_missing.location_id;
        let recovery = plan_one_target(catalog, &target, &files, &mut identity_cache, required)?;
        let Some(recovery) = recovery else {
            continue;
        };
        if let Some(other) = claimed_paths.insert(recovery.path.clone(), target.representation_id)
            && other != target.representation_id
        {
            bail!(
                "one replacement file matched more than one historical photo; choose a narrower folder"
            );
        }
        plan.push(recovery);
    }
    if plan
        .first()
        .is_none_or(|recovery| recovery.missing.location_id != selected_missing.location_id)
    {
        bail!("no matching original was found in the selected folder");
    }
    Ok(plan)
}

/// Plans every safely identifiable missing original owned by one configured
/// Library source. Unlike the photo-anchored sibling flow, this accepts a
/// replacement tree directly: strong identities may match renamed files
/// anywhere below it, while legacy records prefer their original relative
/// path and otherwise require one unique same-name/same-size candidate.
pub(super) fn plan_source_recovery(
    catalog: &CatalogHandle,
    source: &LibrarySourceRecord,
    selected_root: &Path,
) -> AnyResult<PlannedSourceRecovery> {
    let historical_root = native_path_from_location(&source.root)
        .context("decode the historical Library source root")?;
    let source_root_unavailable =
        !std::fs::metadata(&historical_root).is_ok_and(|metadata| metadata.is_dir());
    let targets = catalog.library_source_relink_targets(source.id)?;
    let missing_targets = targets
        .into_iter()
        .filter(|target| !historical_source_is_available(target))
        .collect::<Vec<_>>();
    if missing_targets.is_empty() {
        bail!("this Library folder has no missing originals to locate");
    }

    let files = recursive_file_inventory(selected_root)?;
    let mut identity_cache = HashMap::new();
    let mut claimed_paths = HashMap::<PathBuf, RepresentationId>::new();
    let mut recoveries = Vec::new();
    for target in &missing_targets {
        let same_size = files
            .iter()
            .filter(|file| file.byte_len == target.source.byte_len)
            .cloned()
            .collect::<Vec<_>>();
        let has_strong_identity =
            catalog.representation_has_current_whole_file_identity(target.representation_id)?;
        let preferred_relative_candidate = if has_strong_identity {
            None
        } else {
            native_path_from_location(&target.location)
                .ok()
                .and_then(|path| {
                    path.strip_prefix(&historical_root)
                        .ok()
                        .map(Path::to_path_buf)
                })
                .map(|relative| selected_root.join(relative))
                .filter(|path| same_size.iter().any(|candidate| candidate.path == *path))
        };
        let candidate_slice = preferred_relative_candidate.as_ref().map_or_else(
            || same_size.as_slice(),
            |path| {
                same_size
                    .iter()
                    .position(|candidate| candidate.path == *path)
                    .map_or(&same_size[0..0], |index| &same_size[index..=index])
            },
        );
        let recovery = select_candidate(
            catalog,
            target,
            candidate_slice,
            &mut identity_cache,
            has_strong_identity,
            false,
        )?;
        let Some(recovery) = recovery else {
            continue;
        };
        if let Some(other) = claimed_paths.insert(recovery.path.clone(), target.representation_id)
            && other != target.representation_id
        {
            bail!(
                "one replacement file matched more than one historical photo; choose a narrower folder"
            );
        }
        recoveries.push(recovery);
    }
    if recoveries.is_empty() {
        bail!("no missing originals from this Library source were found in the selected folder");
    }
    Ok(PlannedSourceRecovery {
        recoveries,
        missing_target_count: missing_targets.len(),
        source_root_unavailable,
    })
}

fn find_selected_candidate(
    catalog: &CatalogHandle,
    missing: &MissingSourceLocationRecord,
    folder: &Path,
) -> AnyResult<PlannedRecovery> {
    let has_strong_identity =
        catalog.representation_has_current_whole_file_identity(missing.representation_id)?;
    let candidates = recursive_file_inventory(folder)?
        .into_iter()
        .filter(|candidate| candidate.byte_len == missing.source.byte_len)
        .collect::<Vec<_>>();
    select_candidate(
        catalog,
        missing,
        &candidates,
        &mut HashMap::new(),
        has_strong_identity,
        true,
    )?
    .ok_or_else(|| anyhow!("no matching original was found in the selected folder"))
}

fn recursive_file_inventory(folder: &Path) -> AnyResult<Vec<FileObservation>> {
    let mut pending_directories = vec![folder.to_path_buf()];
    let mut files = Vec::new();
    while let Some(directory) = pending_directories.pop() {
        let mut entries = std::fs::read_dir(&directory)
            .with_context(|| format!("read selected recovery folder {}", directory.display()))?
            .filter_map(Result::ok)
            .collect::<Vec<_>>();
        entries.sort_by_key(std::fs::DirEntry::path);
        let mut child_directories = Vec::new();
        for entry in entries {
            let Ok(file_type) = entry.file_type() else {
                continue;
            };
            if file_type.is_symlink() {
                continue;
            }
            if file_type.is_dir() {
                child_directories.push(entry.path());
            } else if file_type.is_file()
                && let Ok(metadata) = entry.metadata()
            {
                files.push(FileObservation {
                    path: entry.path(),
                    byte_len: metadata.len(),
                });
            }
        }
        pending_directories.extend(child_directories.into_iter().rev());
    }
    files.sort_by(|left, right| left.path.cmp(&right.path));
    Ok(files)
}

fn plan_one_target(
    catalog: &CatalogHandle,
    missing: &MissingSourceLocationRecord,
    files: &[FileObservation],
    identity_cache: &mut HashMap<PathBuf, ContentIdentity>,
    required: bool,
) -> AnyResult<Option<PlannedRecovery>> {
    let has_strong_identity =
        catalog.representation_has_current_whole_file_identity(missing.representation_id)?;
    let same_size = files
        .iter()
        .filter(|file| file.byte_len == missing.source.byte_len)
        .cloned()
        .collect::<Vec<_>>();
    select_candidate(
        catalog,
        missing,
        &same_size,
        identity_cache,
        has_strong_identity,
        required,
    )
}

fn select_candidate(
    catalog: &CatalogHandle,
    missing: &MissingSourceLocationRecord,
    candidates: &[FileObservation],
    identity_cache: &mut HashMap<PathBuf, ContentIdentity>,
    has_strong_identity: bool,
    required: bool,
) -> AnyResult<Option<PlannedRecovery>> {
    if has_strong_identity {
        let mut exact = Vec::new();
        for candidate in candidates {
            let identity = identity_for(&candidate.path, identity_cache)?;
            if catalog
                .relink_match(identity)?
                .is_some_and(|matched| matched.representation_id == missing.representation_id)
            {
                exact.push(candidate.path.clone());
            }
        }
        match exact.as_slice() {
            [path] => {
                return Ok(Some(PlannedRecovery {
                    missing: missing.clone(),
                    path: path.clone(),
                    may_bootstrap_identity: false,
                }));
            }
            [] if required => {
                bail!("no file in the selected folder matched the original's exact identity")
            }
            [] => return Ok(None),
            _ => bail!(
                "more than one exact copy of the historical original was found; choose a narrower folder"
            ),
        }
    }

    let weak = candidates
        .iter()
        .filter(|candidate| same_original_file_name(missing, &candidate.path))
        .collect::<Vec<_>>();
    let candidate = match weak.as_slice() {
        [candidate] => *candidate,
        [] if required => bail!("no matching original was found in the selected folder"),
        [] => return Ok(None),
        _ => {
            bail!("more than one same-name original candidate was found; choose a narrower folder")
        }
    };
    let identity = identity_for(&candidate.path, identity_cache)?;
    match catalog.relink_match(identity)? {
        None => Ok(Some(PlannedRecovery {
            missing: missing.clone(),
            path: candidate.path.clone(),
            may_bootstrap_identity: true,
        })),
        Some(matched) if matched.representation_id == missing.representation_id => {
            Ok(Some(PlannedRecovery {
                missing: missing.clone(),
                path: candidate.path.clone(),
                may_bootstrap_identity: false,
            }))
        }
        Some(_) => bail!(
            "a same-name replacement is already the exact original of a different catalog photo"
        ),
    }
}

fn direct_file_inventory(folder: &Path) -> AnyResult<Vec<FileObservation>> {
    let mut files = std::fs::read_dir(folder)
        .with_context(|| format!("read replacement directory {}", folder.display()))?
        .filter_map(Result::ok)
        .filter_map(|entry| {
            let file_type = entry.file_type().ok()?;
            if file_type.is_symlink() || !file_type.is_file() {
                return None;
            }
            let metadata = entry.metadata().ok()?;
            Some(FileObservation {
                path: entry.path(),
                byte_len: metadata.len(),
            })
        })
        .collect::<Vec<_>>();
    files.sort_by(|left, right| left.path.cmp(&right.path));
    Ok(files)
}

fn identity_for<'a>(
    path: &Path,
    cache: &'a mut HashMap<PathBuf, ContentIdentity>,
) -> AnyResult<&'a ContentIdentity> {
    if !cache.contains_key(path) {
        cache.insert(path.to_path_buf(), whole_file_identity(path)?);
    }
    cache
        .get(path)
        .ok_or_else(|| anyhow!("recovery identity cache did not retain {}", path.display()))
}

pub(super) fn same_original_file_name(
    missing: &MissingSourceLocationRecord,
    candidate: &Path,
) -> bool {
    let Some(candidate_name) = candidate.file_name().and_then(|name| name.to_str()) else {
        return false;
    };
    native_path_from_location(&missing.location)
        .ok()
        .and_then(|path| path.file_name().map(std::ffi::OsStr::to_owned))
        .and_then(|name| name.to_str().map(str::to_owned))
        .is_some_and(|original_name| original_name.eq_ignore_ascii_case(candidate_name))
}

fn has_exact_parent(target: &MissingSourceLocationRecord, expected: &Path) -> bool {
    native_path_from_location(&target.location)
        .ok()
        .and_then(|path| path.parent().map(Path::to_path_buf))
        .is_some_and(|parent| parent == expected)
}

fn historical_source_is_available(target: &MissingSourceLocationRecord) -> bool {
    native_path_from_location(&target.location)
        .ok()
        .and_then(|path| std::fs::metadata(path).ok())
        .is_some_and(|metadata| metadata.is_file())
}

fn whole_file_identity(path: &Path) -> AnyResult<ContentIdentity> {
    let mut file =
        File::open(path).with_context(|| format!("open recovery candidate {}", path.display()))?;
    let mut hasher = blake3::Hasher::new();
    let mut buffer = vec![0_u8; 64 * 1024].into_boxed_slice();
    loop {
        let read = file
            .read(&mut buffer)
            .with_context(|| format!("read recovery candidate {}", path.display()))?;
        if read == 0 {
            break;
        }
        hasher.update(&buffer[..read]);
    }
    Ok(ContentIdentity::whole_file_blake3(
        *hasher.finalize().as_bytes(),
    ))
}
