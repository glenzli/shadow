//! Actor-side execution for photo/source/visual Review read models.

use crate::Catalog;

use super::super::protocol::ReviewProjectionMessage;

pub(super) fn run_review_projection_message(
    catalog: &mut Catalog,
    message: ReviewProjectionMessage,
) {
    match message {
        ReviewProjectionMessage::Page {
            after,
            limit,
            revision,
            recipe_preview_generator,
            response,
        } => {
            let result = match (revision.as_ref(), recipe_preview_generator.as_ref()) {
                (Some(revision), Some(recipe_preview_generator)) => catalog
                    .review_page_with_technical_and_recipe_preview_generator(
                        after.as_ref(),
                        limit,
                        revision,
                        recipe_preview_generator,
                    ),
                (Some(revision), None) => {
                    catalog.review_page_with_technical(after.as_ref(), limit, revision)
                }
                (None, None) => catalog.review_page(after.as_ref(), limit),
                (None, Some(_)) => unreachable!(
                    "a Recipe-preview generator filter requires a technical Review query"
                ),
            };
            let _ = response.send(result);
        }
        ReviewProjectionMessage::ReviewSource {
            photo_id,
            revision,
            response,
        } => {
            let result = revision.as_ref().map_or_else(
                || catalog.review_source(photo_id),
                |revision| catalog.review_source_with_technical(photo_id, revision),
            );
            let _ = response.send(result);
        }
        ReviewProjectionMessage::PhotoSource(photo_id, response) => {
            let _ = response.send(catalog.photo_source(photo_id));
        }
    }
}
