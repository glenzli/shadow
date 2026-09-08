//! Validated, bounded LUT text carried by Recipe v2. Import never opens the
//! source document's paths; it installs verified bytes into the local LUT store.

use std::{
    collections::{BTreeMap, BTreeSet},
    fs::{self, File},
    io::{Read, Write},
    path::Path,
};

use anyhow::{Context, Result, bail};
use sha2::{Digest, Sha256};
use shadow_bridge::{MAX_LUT_DOCUMENT_BYTES, validate_cube_lut_document};
use shadow_domain::{ShadowRecipeDocument, ShadowRecipeLutResource};

use crate::recipe_v1::{GradeStackDraft, LutEditParameters};

pub(crate) type InstalledRecipeLuts = BTreeMap<String, String>;

pub(crate) fn collect_lut_resources(
    draft: &GradeStackDraft,
) -> Result<Vec<ShadowRecipeLutResource>> {
    let mut resources = Vec::new();
    let mut seen = BTreeSet::new();
    let mut total = 0_usize;
    for node in &draft.grade_nodes {
        let lut = &node.fine.lut;
        if lut.resource_id.is_empty() || !seen.insert(&lut.resource_id) {
            continue;
        }
        let path = Path::new(&lut.managed_path);
        if !path.is_absolute()
            || path.extension().and_then(|part| part.to_str()) != Some("cube")
            || path.file_stem().and_then(|part| part.to_str()) != Some(lut.resource_id.as_str())
        {
            bail!("Recipe LUT is not a content-addressed managed resource");
        }
        let bytes = read_bounded_lut(path)?;
        verify_lut(&lut.resource_id, &bytes)?;
        total += bytes.len();
        if total > 32 * 1024 * 1024 {
            bail!("Recipe LUT resources exceed 32 MiB in total");
        }
        resources.push(ShadowRecipeLutResource::new(
            lut.resource_id.clone(),
            String::from_utf8(bytes).context("LUT text must be UTF-8")?,
        )?);
    }
    Ok(resources)
}

pub(crate) fn install_lut_resources(
    document: &ShadowRecipeDocument,
    store: &Path,
) -> Result<InstalledRecipeLuts> {
    // Validate every member before the first write. Domain has already enforced
    // document size, resource count, uniqueness, and the BLAKE3 transport digest.
    for resource in document.lut_resources() {
        verify_lut(resource.resource_id(), resource.document().as_bytes())?;
    }
    let mut installed = BTreeMap::new();
    if document.lut_resources().is_empty() {
        return Ok(installed);
    }
    fs::create_dir_all(store).context("create destination LUT store")?;
    for resource in document.lut_resources() {
        let destination = store.join(format!("{}.cube", resource.resource_id()));
        let bytes = resource.document().as_bytes();
        if !destination.exists() {
            let mut temporary =
                tempfile::NamedTempFile::new_in(store).context("stage bundled LUT")?;
            temporary
                .write_all(bytes)
                .context("write complete bundled LUT")?;
            temporary
                .as_file()
                .sync_all()
                .context("flush bundled LUT")?;
            match temporary.persist_noclobber(&destination) {
                Ok(_) => {}
                Err(error) if error.error.kind() == std::io::ErrorKind::AlreadyExists => {}
                Err(error) => return Err(error.error).context("publish bundled LUT atomically"),
            }
        }
        if read_bounded_lut(&destination)? != bytes {
            bail!("destination managed LUT does not match the bundled content identity");
        }
        installed.insert(
            resource.resource_id().to_owned(),
            destination
                .to_str()
                .context("destination LUT path is not UTF-8")?
                .to_owned(),
        );
    }
    Ok(installed)
}

pub(crate) fn restore_lut(lut: &mut LutEditParameters, installed: &InstalledRecipeLuts) -> bool {
    if let Some(path) = installed.get(&lut.resource_id) {
        lut.managed_path.clone_from(path);
        true
    } else {
        false
    }
}

fn verify_lut(resource_id: &str, bytes: &[u8]) -> Result<()> {
    if format!("{:x}", Sha256::digest(bytes)) != resource_id {
        bail!("bundled LUT SHA-256 does not match its Recipe resource identity");
    }
    validate_cube_lut_document(bytes)
        .context("validate bundled LUT with the production renderer")?;
    Ok(())
}

fn read_bounded_lut(path: &Path) -> Result<Vec<u8>> {
    let file = File::open(path).context("open managed LUT resource")?;
    if !file.metadata()?.is_file() {
        bail!("managed LUT is not a regular file");
    }
    let mut bytes = Vec::new();
    file.take((MAX_LUT_DOCUMENT_BYTES + 1) as u64)
        .read_to_end(&mut bytes)?;
    if bytes.is_empty() || bytes.len() > MAX_LUT_DOCUMENT_BYTES {
        bail!("managed LUT must contain 1 byte through 16 MiB");
    }
    Ok(bytes)
}

#[cfg(test)]
mod tests;
