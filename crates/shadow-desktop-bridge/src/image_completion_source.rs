//! Quiet source-change review for accepted repairs; normal WB is automatic.
use crate::{ffi, recipe_v1::decode_foundation_settings};
use shadow_domain::ImageCompletionSourceContext;

pub(crate) fn image_completion_source_states(
    foundation: &ffi::FfiPhotoFoundationSettings,
    contexts: &Vec<String>,
) -> Vec<u8> {
    let Ok((foundation, denoise)) = decode_foundation_settings(foundation) else {
        return vec![3; contexts.len()];
    };
    contexts
        .iter()
        .map(|json| {
            if json.is_empty() {
                return 0;
            }
            let Ok(source) = serde_json::from_str::<ImageCompletionSourceContext>(json) else {
                return 3;
            };
            if source.validate().is_err() {
                return 3;
            }
            let old_optics = source.foundation.enabled() && source.foundation.optics().enabled();
            let new_optics = foundation.enabled() && foundation.optics().enabled();
            let geometry_changed = old_optics != new_optics
                || (new_optics && source.foundation.optics() != foundation.optics());
            let denoise_changed = source.raw_ai_denoise.is_effective() != denoise.is_effective()
                || (denoise.is_effective() && source.raw_ai_denoise != denoise);
            if geometry_changed || denoise_changed {
                return 3;
            }
            if source.foundation.effective_raw_white_balance()
                == foundation.effective_raw_white_balance()
            {
                return 1;
            }
            if source.color_basis.is_some() { 2 } else { 3 }
        })
        .collect()
}

#[cfg(test)]
mod tests;
