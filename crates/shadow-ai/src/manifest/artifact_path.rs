use std::collections::BTreeMap;

pub(super) const MAX_PORTABLE_PATH_BYTES: usize = 4096;
pub(super) const MAX_PORTABLE_COMPONENT_BYTES: usize = 255;

/// Validates Shadow's intentionally conservative portable v1 package path.
///
/// ASCII-only names avoid filesystem-dependent Unicode normalization aliases.
/// Forward slashes are the only separators; Windows drive, UNC, ADS, device
/// names, forbidden characters, Windows short-name markers, and leading or
/// trailing space/dot aliases are rejected.
pub(super) fn validate(path: &str) -> bool {
    if path.is_empty()
        || path.len() > MAX_PORTABLE_PATH_BYTES
        || !path.is_ascii()
        || path.starts_with('/')
        || path.starts_with('\\')
        || path.contains('\\')
    {
        return false;
    }
    path.split('/').all(valid_component)
}

pub(super) fn register_alias(
    aliases: &mut BTreeMap<String, String>,
    path: &str,
) -> Result<(), String> {
    let key = path.to_ascii_lowercase();
    if let Some(existing) = aliases.get(&key) {
        if existing != path {
            return Err(existing.clone());
        }
    } else {
        aliases.insert(key, path.to_owned());
    }
    Ok(())
}

fn valid_component(component: &str) -> bool {
    if component.is_empty()
        || component == "."
        || component == ".."
        || component.len() > MAX_PORTABLE_COMPONENT_BYTES
        || component.starts_with(' ')
        || component.ends_with('.')
        || component.ends_with(' ')
        || component.bytes().any(|byte| {
            byte < 0x20
                || byte == 0x7f
                || matches!(
                    byte,
                    b'<' | b'>' | b':' | b'"' | b'\\' | b'|' | b'?' | b'*' | b'~'
                )
        })
    {
        return false;
    }

    let stem = component
        .split_once('.')
        .map_or(component, |(stem, _extension)| stem)
        .to_ascii_uppercase();
    !matches!(
        stem.as_str(),
        "CON" | "PRN" | "AUX" | "NUL" | "CONIN$" | "CONOUT$"
    ) && !is_numbered_device(&stem, "COM")
        && !is_numbered_device(&stem, "LPT")
}

fn is_numbered_device(value: &str, prefix: &str) -> bool {
    value
        .strip_prefix(prefix)
        .is_some_and(|suffix| matches!(suffix, "1" | "2" | "3" | "4" | "5" | "6" | "7" | "8" | "9"))
}

#[cfg(test)]
mod tests;
