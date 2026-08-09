//! Desktop-session projection for transient `SigLIP` semantic search.

use std::path::Path;

use anyhow::{Context, Result as AnyResult};
use shadow_ai::InferRuntimeClient;
use shadow_core::{SemanticSearchPolicy, SemanticSearchReport, search_review_semantics};

use super::{DesktopSession, ffi};

impl DesktopSession {
    pub(crate) fn search_semantics(
        &self,
        infer_base_url: &str,
        credential_file: &str,
        query: &str,
        query_revision: &str,
        language: &str,
    ) -> AnyResult<ffi::FfiSemanticSearchReport> {
        let provider =
            InferRuntimeClient::from_credential_file(infer_base_url, Path::new(credential_file))
                .context("configure local semantic-search provider")?;
        let report = search_review_semantics(
            &self.catalog,
            &self.cache_root,
            &provider,
            query,
            query_revision,
            (!language.is_empty()).then_some(language),
            SemanticSearchPolicy::default(),
        )
        .context("search current Library visuals by meaning")?;
        ffi_semantic_search_report(report)
    }
}

fn ffi_semantic_search_report(
    report: SemanticSearchReport,
) -> AnyResult<ffi::FfiSemanticSearchReport> {
    let skipped_items = report
        .skipped
        .no_current_visual
        .saturating_add(report.skipped.unsupported_visual)
        .saturating_add(report.skipped.stale_input)
        .saturating_add(report.skipped.incompatible_embedding_space);
    Ok(ffi::FfiSemanticSearchReport {
        considered_photos: bounded_u32(report.considered_photos, "considered photo count")?,
        embedded_photos: bounded_u32(report.embedded_photos, "embedded photo count")?,
        skipped_items: bounded_u32(skipped_items, "semantic search skipped count")?,
        truncated: report.truncated,
        matches: report
            .matches
            .into_iter()
            .map(|semantic_match| ffi::FfiSemanticSearchMatch {
                photo_id: semantic_match.photo_id.to_string(),
                representation_id: semantic_match.representation_id.to_string(),
                cosine_similarity: semantic_match.cosine_similarity,
            })
            .collect(),
    })
}

fn bounded_u32(value: usize, field: &'static str) -> AnyResult<u32> {
    u32::try_from(value).with_context(|| format!("{field} exceeds the desktop ABI bound"))
}

#[cfg(test)]
mod tests;
