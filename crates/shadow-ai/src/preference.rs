use serde::{Deserialize, Serialize};
use thiserror::Error;

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct FeatureSchema {
    pub extractor_id: String,
    pub extractor_revision: String,
    pub preprocessing_version: String,
    pub dimension: usize,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct FeatureVector {
    pub schema: FeatureSchema,
    pub values: Vec<f64>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct PreferenceExample {
    pub event_id: String,
    pub sequence: u64,
    pub preferred: FeatureVector,
    pub other: FeatureVector,
    pub weight: f64,
}

#[derive(Debug, Copy, Clone, PartialEq, Serialize, Deserialize)]
pub struct TrainingHyperparameters {
    pub learning_rate: f64,
    pub l2_regularization: f64,
    pub maximum_gradient_norm: f64,
}

#[derive(Debug, Copy, Clone, PartialEq, Serialize, Deserialize)]
pub struct TrainingUpdate {
    pub examples_applied: usize,
    pub mean_log_loss: f64,
    pub trained_through_sequence: u64,
}

/// A tiny Bradley-Terry/logistic head over frozen features.
///
/// The feature extractor remains an external, versioned model. This head is
/// deterministic, local, serializable, and cheap enough to update on CPU.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct LinearPreferenceHead {
    pub schema: FeatureSchema,
    pub weights: Vec<f64>,
    pub examples_seen: u64,
    pub trained_through_sequence: u64,
}

impl LinearPreferenceHead {
    /// Creates a zero/cold-start residual head.
    ///
    /// # Errors
    ///
    /// Returns [`PreferenceModelError`] when the feature schema is incomplete.
    pub fn new(schema: FeatureSchema) -> Result<Self, PreferenceModelError> {
        validate_schema(&schema)?;
        Ok(Self {
            weights: vec![0.0; schema.dimension],
            schema,
            examples_seen: 0,
            trained_through_sequence: 0,
        })
    }

    /// Returns an unbounded personal preference residual.
    ///
    /// # Errors
    ///
    /// Rejects mismatched/non-finite features instead of producing a model score.
    pub fn score(&self, features: &FeatureVector) -> Result<f64, PreferenceModelError> {
        validate_features(&self.schema, features)?;
        Ok(dot(&self.weights, &features.values))
    }

    /// Returns the pairwise probability `P(left > right)`.
    ///
    /// # Errors
    ///
    /// Rejects mismatched/non-finite feature vectors.
    pub fn pairwise_probability(
        &self,
        left: &FeatureVector,
        right: &FeatureVector,
    ) -> Result<f64, PreferenceModelError> {
        let difference = feature_difference(&self.schema, left, right)?;
        Ok(sigmoid(dot(&self.weights, &difference)))
    }

    /// Applies deterministic online SGD in `(sequence, event_id)` order.
    ///
    /// The method updates only this small head; it never fine-tunes the frozen
    /// visual feature extractor.
    ///
    /// # Errors
    ///
    /// Returns [`PreferenceModelError`] for invalid hyperparameters, feature
    /// schema mismatches, non-finite values, or already-consumed examples.
    pub fn train_incremental(
        &mut self,
        examples: &[PreferenceExample],
        hyperparameters: TrainingHyperparameters,
    ) -> Result<TrainingUpdate, PreferenceModelError> {
        validate_hyperparameters(hyperparameters)?;
        let mut ordered: Vec<_> = examples.iter().collect();
        ordered.sort_by(|left, right| {
            left.sequence
                .cmp(&right.sequence)
                .then_with(|| left.event_id.cmp(&right.event_id))
        });
        let mut total_loss = 0.0;
        let mut loss_denominator = 0.0;
        let mut applied = 0_usize;
        let mut through = self.trained_through_sequence;
        let mut updated_weights = self.weights.clone();

        for example in ordered {
            if example.sequence <= self.trained_through_sequence {
                return Err(PreferenceModelError::AlreadyConsumed(example.sequence));
            }
            if example.sequence <= through {
                return Err(PreferenceModelError::NonMonotonicSequence(example.sequence));
            }
            if !example.weight.is_finite() || example.weight <= 0.0 {
                return Err(PreferenceModelError::InvalidExampleWeight);
            }
            let difference = feature_difference(&self.schema, &example.preferred, &example.other)?;
            let probability = sigmoid(dot(&updated_weights, &difference));
            total_loss += -probability.max(f64::MIN_POSITIVE).ln() * example.weight;
            loss_denominator += 1.0;
            let mut gradient: Vec<_> = difference
                .iter()
                .zip(&updated_weights)
                .map(|(difference, weight)| {
                    (probability - 1.0) * difference * example.weight
                        + hyperparameters.l2_regularization * weight
                })
                .collect();
            clip_gradient(&mut gradient, hyperparameters.maximum_gradient_norm);
            for (weight, gradient) in updated_weights.iter_mut().zip(gradient) {
                *weight -= hyperparameters.learning_rate * gradient;
            }
            applied += 1;
            through = through.max(example.sequence);
        }

        self.weights = updated_weights;
        self.examples_seen = self.examples_seen.saturating_add(applied as u64);
        self.trained_through_sequence = through;
        Ok(TrainingUpdate {
            examples_applied: applied,
            mean_log_loss: if applied == 0 {
                0.0
            } else {
                total_loss / loss_denominator
            },
            trained_through_sequence: through,
        })
    }
}

fn validate_schema(schema: &FeatureSchema) -> Result<(), PreferenceModelError> {
    if schema.extractor_id.trim().is_empty()
        || schema.extractor_revision.trim().is_empty()
        || schema.preprocessing_version.trim().is_empty()
        || schema.dimension == 0
    {
        return Err(PreferenceModelError::InvalidFeatureSchema);
    }
    Ok(())
}

fn validate_features(
    expected: &FeatureSchema,
    features: &FeatureVector,
) -> Result<(), PreferenceModelError> {
    if &features.schema != expected {
        return Err(PreferenceModelError::FeatureSchemaMismatch);
    }
    if features.values.len() != expected.dimension {
        return Err(PreferenceModelError::FeatureDimensionMismatch {
            expected: expected.dimension,
            actual: features.values.len(),
        });
    }
    if features.values.iter().any(|value| !value.is_finite()) {
        return Err(PreferenceModelError::NonFiniteFeature);
    }
    Ok(())
}

fn feature_difference(
    schema: &FeatureSchema,
    left: &FeatureVector,
    right: &FeatureVector,
) -> Result<Vec<f64>, PreferenceModelError> {
    validate_features(schema, left)?;
    validate_features(schema, right)?;
    Ok(left
        .values
        .iter()
        .zip(&right.values)
        .map(|(left, right)| left - right)
        .collect())
}

fn validate_hyperparameters(
    parameters: TrainingHyperparameters,
) -> Result<(), PreferenceModelError> {
    if !parameters.learning_rate.is_finite()
        || parameters.learning_rate <= 0.0
        || !parameters.l2_regularization.is_finite()
        || parameters.l2_regularization < 0.0
        || !parameters.maximum_gradient_norm.is_finite()
        || parameters.maximum_gradient_norm <= 0.0
    {
        return Err(PreferenceModelError::InvalidHyperparameters);
    }
    Ok(())
}

fn dot(left: &[f64], right: &[f64]) -> f64 {
    left.iter()
        .zip(right)
        .map(|(left, right)| left * right)
        .sum()
}

fn sigmoid(value: f64) -> f64 {
    if value >= 0.0 {
        1.0 / (1.0 + (-value).exp())
    } else {
        let exp = value.exp();
        exp / (1.0 + exp)
    }
}

fn clip_gradient(gradient: &mut [f64], maximum_norm: f64) {
    let norm = gradient
        .iter()
        .map(|value| value * value)
        .sum::<f64>()
        .sqrt();
    if norm > maximum_norm {
        let scale = maximum_norm / norm;
        for value in gradient {
            *value *= scale;
        }
    }
}

#[derive(Debug, Clone, PartialEq, Error)]
pub enum PreferenceModelError {
    #[error("feature schema must pin an extractor, revision, preprocessing, and dimension")]
    InvalidFeatureSchema,
    #[error("feature vector was produced by a different schema")]
    FeatureSchemaMismatch,
    #[error("feature dimension mismatch: expected {expected}, got {actual}")]
    FeatureDimensionMismatch { expected: usize, actual: usize },
    #[error("feature vector contains NaN or infinity")]
    NonFiniteFeature,
    #[error("training hyperparameters must be finite and within their valid ranges")]
    InvalidHyperparameters,
    #[error("training example weight must be finite and greater than zero")]
    InvalidExampleWeight,
    #[error("training example sequence {0} has already been consumed")]
    AlreadyConsumed(u64),
    #[error("training example sequence {0} is duplicated within the incremental batch")]
    NonMonotonicSequence(u64),
}

#[cfg(test)]
mod tests {
    use super::*;

    fn schema() -> FeatureSchema {
        FeatureSchema {
            extractor_id: "frozen-features".into(),
            extractor_revision: "r1".into(),
            preprocessing_version: "preview-v1".into(),
            dimension: 2,
        }
    }

    fn vector(values: [f64; 2]) -> FeatureVector {
        FeatureVector {
            schema: schema(),
            values: values.to_vec(),
        }
    }

    fn hyperparameters() -> TrainingHyperparameters {
        TrainingHyperparameters {
            learning_rate: 0.2,
            l2_regularization: 0.001,
            maximum_gradient_norm: 5.0,
        }
    }

    #[test]
    fn explicit_pairs_incrementally_teach_a_small_local_head() {
        let mut head = LinearPreferenceHead::new(schema()).expect("valid schema");
        let before = head
            .pairwise_probability(&vector([1.0, 0.0]), &vector([0.0, 0.0]))
            .expect("valid features");
        let examples: Vec<_> = (1..=20)
            .map(|sequence| PreferenceExample {
                event_id: format!("event-{sequence}"),
                sequence,
                preferred: vector([1.0, 0.0]),
                other: vector([0.0, 0.0]),
                weight: 1.0,
            })
            .collect();
        let update = head
            .train_incremental(&examples, hyperparameters())
            .expect("train preference head");
        let after = head
            .pairwise_probability(&vector([1.0, 0.0]), &vector([0.0, 0.0]))
            .expect("valid features");
        assert!((before - 0.5).abs() < f64::EPSILON);
        assert!(after > before);
        assert_eq!(update.examples_applied, 20);
        assert_eq!(head.examples_seen, 20);
    }

    #[test]
    fn training_order_is_deterministic() {
        let first = PreferenceExample {
            event_id: "a".into(),
            sequence: 1,
            preferred: vector([1.0, 0.0]),
            other: vector([0.0, 0.0]),
            weight: 1.0,
        };
        let second = PreferenceExample {
            event_id: "b".into(),
            sequence: 2,
            preferred: vector([0.0, 1.0]),
            other: vector([0.0, 0.0]),
            weight: 1.0,
        };
        let mut ordered = LinearPreferenceHead::new(schema()).expect("valid schema");
        ordered
            .train_incremental(&[first.clone(), second.clone()], hyperparameters())
            .expect("train ordered");
        let mut shuffled = LinearPreferenceHead::new(schema()).expect("valid schema");
        shuffled
            .train_incremental(&[second, first], hyperparameters())
            .expect("train shuffled");
        assert_eq!(ordered, shuffled);
    }

    #[test]
    fn a_different_feature_revision_cannot_be_mixed_in() {
        let head = LinearPreferenceHead::new(schema()).expect("valid schema");
        let mut foreign = vector([1.0, 0.0]);
        foreign.schema.extractor_revision = "r2".into();
        assert_eq!(
            head.score(&foreign),
            Err(PreferenceModelError::FeatureSchemaMismatch)
        );
    }

    #[test]
    fn an_invalid_later_example_leaves_the_head_unchanged() {
        let mut head = LinearPreferenceHead::new(schema()).expect("valid schema");
        let before = head.clone();
        let valid = PreferenceExample {
            event_id: "valid".into(),
            sequence: 1,
            preferred: vector([1.0, 0.0]),
            other: vector([0.0, 0.0]),
            weight: 1.0,
        };
        let mut invalid = valid.clone();
        invalid.event_id = "invalid".into();
        invalid.sequence = 2;
        invalid.other.schema.extractor_revision = "wrong".into();
        assert_eq!(
            head.train_incremental(&[valid, invalid], hyperparameters()),
            Err(PreferenceModelError::FeatureSchemaMismatch)
        );
        assert_eq!(head, before);
    }
}
