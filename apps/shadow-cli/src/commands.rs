use super::{backup, catalog, decode, learning, people, remote_library, scan, semantic};
use anyhow::{Result, bail};
use shadow_domain::ImportSessionId;

pub(super) fn run(arguments: impl IntoIterator<Item = String>) -> Result<()> {
    let arguments = arguments.into_iter().collect::<Vec<_>>();
    if learning::run_if_requested(&arguments)?
        || people::run_if_requested(&arguments)?
        || semantic::run_if_requested(&arguments)?
    {
        return Ok(());
    }
    run_positional_command(&arguments)
}

fn run_positional_command(arguments: &[String]) -> Result<()> {
    match arguments {
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
        [command, backup_path, catalog_path] if command == "restore-backup" => {
            backup::restore(backup_path, catalog_path)?;
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
        [
            command,
            catalog_path,
            cache_root,
            folder,
            server_state_root,
            bind_address,
            token_file,
            display_name,
        ] if command == "library-serve" => {
            remote_library::serve(&remote_library::ServeOptions {
                catalog_path,
                cache_root,
                folder,
                server_state_root,
                bind_address,
                token_file,
                display_name,
            })?;
        }
        [
            command,
            server_address,
            token_file,
            mirror_root,
            preview_cache_root,
        ] if command == "library-sync" => {
            remote_library::sync(server_address, token_file, mirror_root, preview_cache_root)?;
        }
        [
            command,
            server_address,
            token_file,
            mirror_root,
            original_cache_root,
            local_catalog_path,
            remote_photo_id,
            remote_representation_id,
        ] if command == "library-materialize" => {
            remote_library::materialize(&remote_library::MaterializeOptions {
                server_address,
                token_file,
                mirror_root,
                original_cache_root,
                local_catalog_path,
                remote_photo_id,
                remote_representation_id,
            })?;
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
        "usage:\n  shadow-cli learning-report <catalog.sqlite> <global|project:id> <after-sequence>\n  shadow-cli learning-confirm-edit <catalog.sqlite> <scope> <photo-id> <baseline-commit> <approved-commit> <manual|imported|assisted|mixed|unknown> <style|correction>\n  shadow-cli learning-forget <catalog.sqlite> <event-id>\n  shadow-cli init <catalog.sqlite>\n  shadow-cli scan <catalog.sqlite> <folder>\n  shadow-cli scan-cache <catalog.sqlite> <cache-root> <folder>\n  shadow-cli cache-read <catalog.sqlite> <cache-root> <path>\n  shadow-cli resume <catalog.sqlite> <session-id>\n  shadow-cli recoverable <catalog.sqlite>\n  shadow-cli stats <catalog.sqlite>\n  shadow-cli backup <catalog.sqlite> <backup.sqlite>\n  shadow-cli verify-backup <backup.sqlite>\n  shadow-cli restore-backup <backup.sqlite> <offline-catalog.sqlite>\n  shadow-cli inspect-raw <path>\n  shadow-cli inspect-store <catalog.sqlite> <cache-root> <path>\n  shadow-cli people-cluster <catalog.sqlite> <cache-root> <infer-base-url> <token-file>\n  shadow-cli semantic-search <catalog.sqlite> <cache-root> <infer-base-url> <token-file> <query> [language]\n  shadow-cli library-serve <catalog.sqlite> <preview-cache> <folder> <server-state> <bind-address> <token-file> <display-name>\n  shadow-cli library-sync <server-address> <token-file> <mirror-root> <preview-cache>\n  shadow-cli library-materialize <server-address> <token-file> <mirror-root> <original-cache> <local-catalog.sqlite> <remote-photo-id> <remote-representation-id>"
    );
}
