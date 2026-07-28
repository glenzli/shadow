use super::{backup, catalog, decode, scan};
use anyhow::{Result, bail};
use shadow_domain::ImportSessionId;

pub(super) fn run(arguments: impl IntoIterator<Item = String>) -> Result<()> {
    let arguments = arguments.into_iter().collect::<Vec<_>>();
    match arguments.as_slice() {
        [command, catalog_path] if command == "init" => {
            catalog::initialize(catalog_path)?;
        }
        [command, catalog_path] if command == "stats" => {
            catalog::print_catalog_stats(catalog_path)?;
        }
        [command, catalog_path] if command == "recoverable" => {
            catalog::print_recoverable_sessions(catalog_path)?;
        }
        [command, catalog_path, backup_path] if command == "backup" => {
            backup::create(catalog_path, backup_path)?;
        }
        [command, backup_path] if command == "verify-backup" => {
            backup::verify(backup_path)?;
        }
        [command, raw_path] if command == "inspect-raw" => {
            decode::inspect_raw(raw_path)?;
        }
        [command, catalog_path, cache_root, raw_path] if command == "inspect-store" => {
            decode::inspect_store(catalog_path, cache_root, raw_path)?;
        }
        [command, catalog_path, cache_root, raw_path] if command == "cache-read" => {
            decode::read_cache(catalog_path, cache_root, raw_path)?;
        }
        [command, catalog_path, folder] if command == "scan" => {
            scan::folder(catalog_path, folder)?;
        }
        [command, catalog_path, cache_root, folder] if command == "scan-cache" => {
            scan::folder_with_cache(catalog_path, cache_root, folder)?;
        }
        [command, catalog_path, session_id] if command == "resume" => {
            let session_id: ImportSessionId = session_id
                .parse()
                .map_err(anyhow::Error::msg)
                .map_err(|error| error.context(format!("parse import session id {session_id}")))?;
            scan::resume(catalog_path, session_id)?;
        }
        _ => {
            print_usage();
            bail!("invalid arguments");
        }
    }
    Ok(())
}

fn print_usage() {
    eprintln!(
        "usage:\n  shadow-cli init <catalog.sqlite>\n  shadow-cli scan <catalog.sqlite> <folder>\n  shadow-cli scan-cache <catalog.sqlite> <cache-root> <folder>\n  shadow-cli cache-read <catalog.sqlite> <cache-root> <path>\n  shadow-cli resume <catalog.sqlite> <session-id>\n  shadow-cli recoverable <catalog.sqlite>\n  shadow-cli stats <catalog.sqlite>\n  shadow-cli backup <catalog.sqlite> <backup.sqlite>\n  shadow-cli verify-backup <backup.sqlite>\n  shadow-cli inspect-raw <path>\n  shadow-cli inspect-store <catalog.sqlite> <cache-root> <path>"
    );
}
