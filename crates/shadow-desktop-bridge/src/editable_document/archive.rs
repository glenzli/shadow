use std::{
    collections::BTreeSet,
    fs::{self, File, FileTimes, OpenOptions},
    io::{Read, Write},
    path::{Component, Path, PathBuf},
    time::{Duration, UNIX_EPOCH},
};

use anyhow::{Context, Result, ensure};
use rusqlite::{Connection, params};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use shadow_domain::AssetLocation;
use shadow_native_path::{native_location, native_path_from_location};
use zip::{CompressionMethod, ZipArchive, ZipWriter, write::SimpleFileOptions};

use crate::{ffi, native_path_ffi};

const FORMAT: &str = "dev.shadow.document";
const MAX_ENTRIES: usize = 100_000;
const MAX_BYTES: u64 = 64 * 1024 * 1024 * 1024;
const MAX_MANIFEST: u64 = 32 * 1024 * 1024;

#[derive(Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Manifest {
    format: String,
    version: u32,
    revision: String,
    document_id: String,
    sources: Vec<Source>,
    files: Vec<Member>,
}

#[derive(Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Source {
    location_id: Vec<u8>,
    photo_id: String,
    representation_id: String,
    title: String,
    path: String,
    render: String,
    recipe: String,
    history: String,
    modified_at_ms: Option<i64>,
}

#[derive(Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Member {
    path: String,
    bytes: u64,
    sha256: String,
}

pub(crate) fn pack_editable_document(
    root: &ffi::FfiNativePath,
    output: &ffi::FfiNativePath,
    replace_sha256: &str,
) -> Result<String> {
    pack(
        &native_path_ffi::path_from_ffi(root)?,
        &native_path_ffi::path_from_ffi(output)?,
        replace_sha256,
    )
}

pub(super) fn pack(root: &Path, output: &Path, replace_sha256: &str) -> Result<String> {
    ensure!(
        root.join(".shadow-document-session").is_file(),
        "not an independent document session"
    );
    // The lock lives in the per-user temporary directory, so the deliverable
    // stays one file. Kernel ownership is released even after a process crash.
    let lock_root = std::env::temp_dir().join("shadow-document-publication-locks");
    fs::create_dir_all(&lock_root)?;
    let parent = output
        .parent()
        .context("document output needs a parent")?
        .canonicalize()?;
    let key = parent
        .join(output.file_name().context("document filename missing")?)
        .to_string_lossy()
        .to_lowercase();
    let lock = OpenOptions::new()
        .read(true)
        .write(true)
        .create(true)
        .truncate(false)
        .open(lock_root.join(blake3::hash(key.as_bytes()).to_hex().to_string()))?;
    lock.try_lock()
        .context("another editor is saving this document")?;
    if replace_sha256.is_empty() {
        ensure!(
            !output.exists() && !output.is_symlink(),
            "document destination already exists; use Save As"
        );
    } else {
        regular(output)?;
        ensure!(
            digest_file(output)? == replace_sha256,
            "document changed since opening; use Save As"
        );
    }
    let staging = tempfile::tempdir()?;
    shadow_catalog::create_catalog_backup(
        &root.join("catalog.sqlite"),
        &staging.path().join("catalog.sqlite"),
    )?;
    let catalog = Connection::open(staging.path().join("catalog.sqlite"))?;
    let mut sources = Vec::new();
    let mut source_files = Vec::new();
    let titles: std::collections::BTreeMap<String, String> =
        fs::read(root.join("document-source-names.json"))
            .ok()
            .map(|bytes| serde_json::from_slice(&bytes))
            .transpose()?
            .unwrap_or_default();
    let mut query = catalog.prepare("SELECT l.id,l.platform,l.native_path,l.display_path,r.photo_id,r.id,r.modified_at_ms,r.byte_len FROM locations l JOIN representations r ON r.id=l.representation_id ORDER BY l.id")?;
    let mut rows = query.query([])?;
    while let Some(row) = rows.next()? {
        ensure!(sources.len() < 256, "document exceeds 256 sources");
        let location = AssetLocation {
            platform: shadow_native_path::current_platform(),
            native_path: row.get(2)?,
            display_path: row.get(3)?,
        };
        let source = native_path_from_location(&location)?;
        regular(&source)?;
        let photo_id = uuid::Uuid::from_slice(&row.get::<_, Vec<u8>>(4)?)?.to_string();
        let representation_id = uuid::Uuid::from_slice(&row.get::<_, Vec<u8>>(5)?)?.to_string();
        let extension = source
            .extension()
            .and_then(|v| v.to_str())
            .context("source has no supported extension")?
            .to_ascii_lowercase();
        ensure!(
            extension.len() <= 10 && extension.bytes().all(|v| v.is_ascii_alphanumeric()),
            "invalid source extension"
        );
        let path = format!("sources/{representation_id}.{extension}");
        let modified_at_ms: Option<i64> = row.get(6)?;
        ensure!(
            fs::metadata(&source)?.len() == u64::try_from(row.get::<_, i64>(7)?)?,
            "original changed before document save"
        );
        let render = format!("renders/{photo_id}.png");
        let recipe = format!("recipes/{photo_id}.json");
        let history = format!("histories/{photo_id}.json");
        regular(&root.join(&render)).context("document render is missing")?;
        source_files.push((path.clone(), source));
        let title = titles.get(&representation_id).cloned().unwrap_or_else(|| {
            Path::new(&location.display_path)
                .file_name()
                .unwrap_or_default()
                .to_string_lossy()
                .into_owned()
        });
        sources.push(Source {
            location_id: row.get(0)?,
            photo_id,
            representation_id,
            title,
            path,
            render,
            recipe,
            history,
            modified_at_ms,
        });
    }
    drop(rows);
    drop(query);
    ensure!(!sources.is_empty(), "document has no sources");
    // The archived Catalog contains relative locators only. Reopening binds all
    // of them to the verified private extraction directory before Catalog use.
    let mut resources = super::resources::ResourceClosure::new(root);
    for source in &sources {
        let original = source_files
            .iter()
            .find(|(name, _)| name == &source.path)
            .context("document source missing")?
            .1
            .clone();
        relocate(&catalog, source, Path::new(&source.path))?;
        let id = uuid::Uuid::parse_str(&source.photo_id)?;
        let mut query = catalog.prepare(
            "SELECT commit_json FROM recipe_commits WHERE photo_id=?1 ORDER BY created_at_ms,id",
        )?;
        let mut commits = Vec::new();
        for row in query.query_map([id.as_bytes().as_slice()], |r| r.get::<_, String>(0))? {
            let commit: shadow_domain::RecipeCommit = serde_json::from_str(&row?)?;
            commit.validate()?;
            resources.validate(commit.snapshot(), &original)?;
            commits.push(commit);
        }
        let mut refs = catalog.prepare(
            "SELECT name,kind,commit_id FROM recipe_refs WHERE photo_id=?1 ORDER BY name",
        )?;
        let mut references = Vec::new();
        for row in refs.query_map([id.as_bytes().as_slice()], |r| {
            Ok((
                r.get::<_, String>(0)?,
                r.get::<_, String>(1)?,
                r.get::<_, Vec<u8>>(2)?,
            ))
        })? {
            let (name, kind, id) = row?;
            references.push(serde_json::json!({"name":name,"kind":kind,"commitId":uuid::Uuid::from_slice(&id)?.to_string()}));
        }
        let recipe_path = staging.path().join(&source.recipe);
        fs::create_dir_all(recipe_path.parent().context("recipe parent missing")?)?;
        fs::write(
            &recipe_path,
            serde_json::to_vec(
                &serde_json::json!({"format":"dev.shadow.document-recipes","version":1,"photoId":source.photo_id,"commits":commits,"refs":references}),
            )?,
        )?;
        source_files.push((source.recipe.clone(), recipe_path));
        let history_path = staging.path().join(&source.history);
        fs::create_dir_all(history_path.parent().context("history parent missing")?)?;
        super::history::export_history(
            root,
            &source.photo_id,
            &history_path,
            &mut resources,
            &original,
        )?;
        source_files.push((source.history.clone(), history_path));
    }
    drop(catalog);
    let mut files = vec![(
        "catalog.sqlite".to_owned(),
        staging.path().join("catalog.sqlite"),
    )];
    for directory in [
        "cache/blobs",
        "derived-rasters/objects",
        "cache/ai/raw-foundations/foundations",
        "cache/ai/raw-foundations/aliases",
        "lut-store",
        "optics-profiles",
        "document-history",
        "renders",
    ] {
        inventory(root, Path::new(directory), &mut files)?;
    }
    source_files.append(&mut files);
    files = source_files;
    files.sort_by(|a, b| a.0.cmp(&b.0));
    ensure!(
        files.len() <= MAX_ENTRIES,
        "document has too many resources"
    );
    let parent = output.parent().context("document output needs a parent")?;
    let mut temporary = tempfile::NamedTempFile::new_in(parent)?;
    let mut writer = ZipWriter::new(temporary.as_file_mut());
    let mut members = Vec::new();
    let mut total = 0_u64;
    for (name, path) in &files {
        safe_name(name)?;
        let options = SimpleFileOptions::default()
            .compression_method(
                if name.starts_with("sources/") || name.starts_with("renders/") {
                    CompressionMethod::Stored
                } else {
                    CompressionMethod::Deflated
                },
            )
            .large_file(true)
            .unix_permissions(0o600);
        writer.start_file(name, options)?;
        let mut input = File::open(path)?;
        let (bytes, sha256) = transfer(&mut input, &mut writer, MAX_BYTES - total)?;
        total += bytes;
        members.push(Member {
            path: name.clone(),
            bytes,
            sha256,
        });
    }
    let document_id = fs::read_to_string(root.join(".shadow-document-session"))?;
    uuid::Uuid::parse_str(&document_id)?;
    let manifest = Manifest {
        format: FORMAT.into(),
        version: 1,
        revision: uuid::Uuid::now_v7().to_string(),
        document_id,
        sources,
        files: members,
    };
    let bytes = serde_json::to_vec_pretty(&manifest)?;
    ensure!(
        bytes.len() as u64 <= MAX_MANIFEST,
        "document manifest is too large"
    );
    writer.start_file(
        "manifest.json",
        SimpleFileOptions::default().compression_method(CompressionMethod::Deflated),
    )?;
    writer.write_all(&bytes)?;
    writer.finish()?;
    temporary.as_file().sync_all()?;
    // Re-read the complete staged archive before its single publication point.
    verify_archive(temporary.path(), None)?;
    let digest = digest_file(temporary.path())?;
    if replace_sha256.is_empty() {
        temporary
            .persist_noclobber(output)
            .map_err(|e| e.error)
            .context("document destination already exists or cannot be published")?;
    } else {
        regular(output)?;
        ensure!(
            digest_file(output)? == replace_sha256,
            "document changed since it was opened or saved; use Save As"
        );
        temporary.persist(output).map_err(|e| e.error)?;
    }
    #[cfg(unix)]
    File::open(parent)?.sync_all()?;
    Ok(digest)
}

pub(crate) fn unpack_editable_document(
    source: &ffi::FfiNativePath,
    root: &ffi::FfiNativePath,
) -> Result<Vec<ffi::FfiPipelineInput>> {
    unpack(
        &native_path_ffi::path_from_ffi(source)?,
        &native_path_ffi::path_from_ffi(root)?,
    )
}

pub(super) fn unpack(source: &Path, root: &Path) -> Result<Vec<ffi::FfiPipelineInput>> {
    ensure!(
        fs::read_dir(root)?.next().is_none(),
        "document extraction directory must be empty"
    );
    let manifest = verify_archive(source, Some(root))?;
    shadow_catalog::verify_catalog_backup(&root.join("catalog.sqlite"))?;
    let catalog = Connection::open(root.join("catalog.sqlite"))?;
    let count: i64 = catalog.query_row("SELECT count(*) FROM locations", [], |row| row.get(0))?;
    ensure!(
        count == manifest.sources.len() as i64,
        "document source set differs from its Catalog"
    );
    let mut inputs = Vec::new();
    for source in &manifest.sources {
        let path = root.join(&source.path);
        let identity: (Vec<u8>, Vec<u8>, Option<i64>) = catalog.query_row("SELECT r.photo_id,r.id,r.modified_at_ms FROM locations l JOIN representations r ON l.representation_id=r.id WHERE l.id=?1", [&source.location_id], |r| Ok((r.get(0)?,r.get(1)?,r.get(2)?)))?;
        ensure!(
            uuid::Uuid::from_slice(&identity.0)?.to_string() == source.photo_id
                && uuid::Uuid::from_slice(&identity.1)?.to_string() == source.representation_id
                && identity.2 == source.modified_at_ms,
            "document source identity mismatch"
        );
        if let Some(ms) = source.modified_at_ms {
            ensure!(ms >= 0, "unsupported source timestamp");
            File::options().write(true).open(&path)?.set_times(
                FileTimes::new().set_modified(UNIX_EPOCH + Duration::from_millis(ms as u64)),
            )?;
        }
        relocate(&catalog, source, &path)?;
        inputs.push(ffi::FfiPipelineInput {
            photo_id: source.photo_id.clone(),
            representation_id: source.representation_id.clone(),
            source_path: path
                .to_str()
                .context("document source path is not UTF-8")?
                .into(),
            title: source.title.clone(),
        });
    }
    // A saved document never resumes an old output job or writes old paths.
    catalog.execute_batch("UPDATE export_items SET state='cancelled',finished_at_ms=updated_at_ms WHERE state NOT IN ('completed','cancelled','failed'); UPDATE export_jobs SET state='cancelled',finished_at_ms=updated_at_ms WHERE state NOT IN ('completed','cancelled','failed');")?;
    drop(catalog);
    fs::write(root.join(".shadow-document-session"), &manifest.document_id)?;
    let titles = manifest
        .sources
        .iter()
        .map(|s| (&s.representation_id, &s.title))
        .collect::<std::collections::BTreeMap<_, _>>();
    fs::write(
        root.join("document-source-names.json"),
        serde_json::to_vec(&titles)?,
    )?;
    fs::create_dir_all(root.join("cache"))?;
    Ok(inputs)
}

fn relocate(catalog: &Connection, source: &Source, path: &Path) -> Result<()> {
    let location = native_location(path);
    ensure!(catalog.execute("UPDATE locations SET platform=?1,native_path=?2,display_path=?3,status='online' WHERE id=?4", params![location.platform.as_str(),location.native_path,location.display_path,source.location_id])? == 1, "document source location missing");
    Ok(())
}

fn verify_archive(path: &Path, destination: Option<&Path>) -> Result<Manifest> {
    regular(path)?;
    let mut zip = ZipArchive::new(File::open(path)?)?;
    ensure!(
        zip.len() > 1 && zip.len() <= MAX_ENTRIES + 1,
        "document entry count is invalid"
    );
    let mut seen = BTreeSet::new();
    for i in 0..zip.len() {
        let entry = zip.by_index(i)?;
        safe_name(entry.name())?;
        ensure!(
            !entry.is_dir()
                && !entry.is_symlink()
                && !entry.encrypted()
                && seen.insert(entry.name().to_ascii_lowercase()),
            "document contains duplicate, encrypted or nonregular entries"
        );
    }
    let mut bytes = Vec::new();
    zip.by_name("manifest.json")?
        .take(MAX_MANIFEST + 1)
        .read_to_end(&mut bytes)?;
    ensure!(
        bytes.len() as u64 <= MAX_MANIFEST,
        "document manifest exceeds its limit"
    );
    let manifest: Manifest = serde_json::from_slice(&bytes)?;
    ensure!(
        manifest.format == FORMAT && manifest.version == 1,
        "unsupported Shadow document version"
    );
    uuid::Uuid::parse_str(&manifest.revision)?;
    uuid::Uuid::parse_str(&manifest.document_id)?;
    ensure!(
        !manifest.sources.is_empty() && manifest.sources.len() <= 256,
        "document sources are invalid"
    );
    ensure!(
        manifest.files.len() + 1 == zip.len(),
        "document inventory is incomplete"
    );
    let mut expected = BTreeSet::from(["manifest.json".to_owned()]);
    let mut total = 0_u64;
    for member in &manifest.files {
        safe_name(&member.path)?;
        ensure!(
            allowed_member(&member.path) && expected.insert(member.path.to_ascii_lowercase()),
            "invalid document resource or duplicate inventory"
        );
        total = total
            .checked_add(member.bytes)
            .context("document size overflow")?;
        ensure!(
            total <= MAX_BYTES && member.sha256.len() == 64,
            "document exceeds resource limits"
        );
        let mut entry = zip.by_name(&member.path)?;
        ensure!(
            entry.size() == member.bytes,
            "document entry length mismatch"
        );
        let (bytes, digest) = if let Some(root) = destination {
            let path = root.join(&member.path);
            fs::create_dir_all(path.parent().context("resource parent missing")?)?;
            let mut output = OpenOptions::new().write(true).create_new(true).open(path)?;
            let value = transfer(&mut entry, &mut output, member.bytes)?;
            output.sync_all()?;
            value
        } else {
            transfer(&mut entry, &mut std::io::sink(), member.bytes)?
        };
        ensure!(
            bytes == member.bytes && digest == member.sha256,
            "document resource failed SHA-256 verification"
        );
    }
    ensure!(
        expected == seen && expected.contains("catalog.sqlite"),
        "document file set mismatch"
    );
    let mut ids = BTreeSet::new();
    for source in &manifest.sources {
        ensure!(
            source.title.len() <= 1024 && !source.title.chars().any(char::is_control),
            "invalid document source title"
        );
        uuid::Uuid::parse_str(&source.photo_id)?;
        uuid::Uuid::parse_str(&source.representation_id)?;
        ensure!(
            source.location_id.len() == 16
                && ids.insert(&source.location_id)
                && source.path.starts_with("sources/")
                && source.render.starts_with("renders/")
                && source.recipe.starts_with("recipes/")
                && source.history.starts_with("histories/")
                && [
                    &source.path,
                    &source.render,
                    &source.recipe,
                    &source.history
                ]
                .into_iter()
                .all(|v| expected.contains(&v.to_ascii_lowercase())),
            "document source/render inventory is incomplete"
        );
    }
    Ok(manifest)
}

fn allowed_member(name: &str) -> bool {
    name == "catalog.sqlite"
        || [
            "sources/",
            "renders/",
            "recipes/",
            "histories/",
            "document-history/",
            "cache/blobs/",
            "derived-rasters/",
            "cache/ai/raw-foundations/foundations/",
            "cache/ai/raw-foundations/aliases/",
            "lut-store/",
            "optics-profiles/",
        ]
        .iter()
        .any(|prefix| name.starts_with(prefix))
}

fn safe_name(name: &str) -> Result<()> {
    ensure!(
        !name.is_empty()
            && name.len() <= 512
            && !name.contains(['\\', ':', '\0'])
            && Path::new(name)
                .components()
                .all(|c| matches!(c, Component::Normal(_)))
            && !name
                .split('/')
                .any(|p| p.is_empty() || p == "." || p == ".."),
        "unsafe document entry name"
    );
    Ok(())
}

fn regular(path: &Path) -> Result<()> {
    ensure!(
        fs::symlink_metadata(path)?.file_type().is_file(),
        "document resource is not a regular file"
    );
    Ok(())
}

fn inventory(root: &Path, relative: &Path, files: &mut Vec<(String, PathBuf)>) -> Result<()> {
    let path = root.join(relative);
    if !path.exists() {
        return Ok(());
    }
    ensure!(
        relative.components().count() <= 32 && files.len() < MAX_ENTRIES,
        "document resource traversal limit exceeded"
    );
    let kind = fs::symlink_metadata(&path)?.file_type();
    if kind.is_dir() {
        for item in fs::read_dir(path)? {
            inventory(root, &relative.join(item?.file_name()), files)?;
        }
    } else {
        regular(&path)?;
        let name = relative
            .to_str()
            .context("resource name is not UTF-8")?
            .replace('\\', "/");
        ensure!(allowed_member(&name), "unsupported document resource");
        files.push((name, path));
    }
    Ok(())
}

fn transfer(input: &mut impl Read, output: &mut impl Write, limit: u64) -> Result<(u64, String)> {
    let mut buffer = vec![0; 256 * 1024];
    let mut bytes = 0_u64;
    let mut hash = Sha256::new();
    loop {
        let count = input.read(&mut buffer)?;
        if count == 0 {
            break;
        }
        bytes = bytes
            .checked_add(count as u64)
            .context("resource length overflow")?;
        ensure!(bytes <= limit, "document resource exceeds declared limit");
        output.write_all(&buffer[..count])?;
        hash.update(&buffer[..count]);
    }
    Ok((bytes, format!("{:x}", hash.finalize())))
}

pub(super) fn digest_file(path: &Path) -> Result<String> {
    Ok(transfer(&mut File::open(path)?, &mut std::io::sink(), MAX_BYTES)?.1)
}
