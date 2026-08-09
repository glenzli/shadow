//! Operator entry for the bounded, session-only semantic-search slice.

use std::path::Path;

use anyhow::{Context, Result};
use shadow_ai::InferRuntimeClient;
use shadow_core::{SemanticSearchPolicy, search_review_semantics};

use super::catalog;

struct SemanticSearchOptions<'a> {
    catalog_path: &'a str,
    cache_root: &'a str,
    infer_base_url: &'a str,
    token_file: &'a str,
    query: &'a str,
    language: Option<&'a str>,
}

pub(super) fn run_if_requested(arguments: &[String]) -> Result<bool> {
    let options = match arguments {
        [
            command,
            catalog_path,
            cache_root,
            infer_base_url,
            token_file,
            query,
        ] if command == "semantic-search" => SemanticSearchOptions {
            catalog_path,
            cache_root,
            infer_base_url,
            token_file,
            query,
            language: None,
        },
        [
            command,
            catalog_path,
            cache_root,
            infer_base_url,
            token_file,
            query,
            language,
        ] if command == "semantic-search" => SemanticSearchOptions {
            catalog_path,
            cache_root,
            infer_base_url,
            token_file,
            query,
            language: Some(language),
        },
        _ => return Ok(false),
    };
    search(&options)?;
    Ok(true)
}

fn search(options: &SemanticSearchOptions<'_>) -> Result<()> {
    let actor = catalog::open(options.catalog_path)?;
    let provider = InferRuntimeClient::from_credential_file(
        options.infer_base_url,
        Path::new(options.token_file),
    )
    .context("configure the local infer-runtime semantic provider")?;
    let report = search_review_semantics(
        &actor.handle(),
        options.cache_root,
        &provider,
        options.query,
        "shadow:semantic-query:v1",
        options.language,
        SemanticSearchPolicy::default(),
    )
    .context("search a bounded Review sample with local semantic embeddings")?;
    println!("{}", serde_json::to_string_pretty(&report)?);
    actor.shutdown()?;
    Ok(())
}
