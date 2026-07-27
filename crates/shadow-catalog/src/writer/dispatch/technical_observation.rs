//! Actor-side execution for exact visual-quality evidence.

use crate::Catalog;

use super::super::protocol::TechnicalObservationMessage;

pub(super) fn run_technical_observation_message(
    catalog: &mut Catalog,
    message: TechnicalObservationMessage,
) {
    match message {
        TechnicalObservationMessage::Record(request, response) => {
            let _ = response.send(catalog.record_technical_observation(request.as_ref()));
        }
        TechnicalObservationMessage::Read {
            representation_id,
            expected_source,
            expected_artifact,
            revision,
            response,
        } => {
            let _ = response.send(catalog.technical_observation(
                representation_id,
                expected_source,
                expected_artifact.as_ref(),
                &revision,
            ));
        }
    }
}
