//! Operator entry for the first transient anonymous-person vertical slice.

use std::path::Path;

use anyhow::{Context, Result};
use shadow_ai::InferRuntimeClient;
use shadow_core::{PeopleAnalysisPolicy, analyze_review_people};

use super::catalog;

struct PeopleClusterOptions<'a> {
    catalog_path: &'a str,
    cache_root: &'a str,
    infer_base_url: &'a str,
    token_file: &'a str,
}

pub(super) fn run_if_requested(arguments: &[String]) -> Result<bool> {
    let [
        command,
        catalog_path,
        cache_root,
        infer_base_url,
        token_file,
    ] = arguments
    else {
        return Ok(false);
    };
    if command != "people-cluster" {
        return Ok(false);
    }
    cluster(&PeopleClusterOptions {
        catalog_path,
        cache_root,
        infer_base_url,
        token_file,
    })?;
    Ok(true)
}

fn cluster(options: &PeopleClusterOptions<'_>) -> Result<()> {
    let actor = catalog::open(options.catalog_path)?;
    let provider = InferRuntimeClient::from_credential_file(
        options.infer_base_url,
        Path::new(options.token_file),
    )
    .context("configure the local infer-runtime face provider")?;
    let report = analyze_review_people(
        &actor.handle(),
        options.cache_root,
        &provider,
        PeopleAnalysisPolicy::default(),
    )
    .context("analyze current Review visuals for anonymous people")?;
    println!("{}", serde_json::to_string_pretty(&report)?);
    actor.shutdown()?;
    Ok(())
}
