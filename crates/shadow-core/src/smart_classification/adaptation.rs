//! Lightweight, rebuildable personalization over frozen semantic embeddings.
//!
//! The prompt embedding remains the durable zero-shot prior. Explicit feedback
//! contributes a Tip-Adapter-style local cache immediately; once both classes
//! are represented, a regularized linear residual learns a broader boundary
//! without mutating or fine-tuning the embedding model.

const CACHE_BETA: f32 = 5.0;
const CACHE_MAXIMUM_SHIFT: f32 = 0.018;
const LINEAR_SCORE_BAND: f32 = 0.0125;
const LINEAR_MAXIMUM_LOGIT_SHIFT: f32 = 1.5;
const MINIMUM_LINEAR_EXAMPLES_PER_CLASS: usize = 4;
const LINEAR_ITERATIONS: usize = 96;
const LINEAR_LEARNING_RATE: f32 = 0.16;
const LINEAR_REGULARIZATION: f32 = 0.10;

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub(super) enum FeedbackLabel {
    DoesNotBelong,
    Belongs,
}

impl FeedbackLabel {
    fn target(self) -> f32 {
        match self {
            Self::DoesNotBelong => 0.0,
            Self::Belongs => 1.0,
        }
    }
}

#[derive(Debug, Clone)]
pub(super) struct FeedbackExample {
    pub values: Vec<f32>,
    pub label: FeedbackLabel,
    /// User evidence is `1.0`; future model-assisted evidence must remain
    /// strictly weaker so it cannot numerically replace explicit feedback.
    pub weight: f32,
}

#[derive(Debug, Copy, Clone, PartialEq)]
pub(super) struct AdaptiveScore {
    pub zero_shot_similarity: f32,
    pub adapted_similarity: f32,
}

#[derive(Debug, Clone)]
struct LinearResidual {
    weights: Vec<f32>,
    bias: f32,
    confidence: f32,
}

#[derive(Debug, Clone)]
pub(super) struct AdaptiveCategoryModel {
    prompt: Vec<f32>,
    feedback: Vec<FeedbackExample>,
    linear: Option<LinearResidual>,
}

impl AdaptiveCategoryModel {
    pub fn new(prompt: &[f32], threshold: f32, feedback: Vec<FeedbackExample>) -> Self {
        let linear = train_linear_residual(prompt, threshold, &feedback);
        Self {
            prompt: prompt.to_vec(),
            feedback,
            linear,
        }
    }

    pub fn score(&self, image: &[f32]) -> AdaptiveScore {
        let zero_shot_similarity = dot(&self.prompt, image);
        let cache_shift = CACHE_MAXIMUM_SHIFT * cache_vote(image, &self.feedback);
        let linear_shift = self.linear.as_ref().map_or(0.0, |linear| {
            let logit = (dot(&linear.weights, image) + linear.bias)
                .clamp(-LINEAR_MAXIMUM_LOGIT_SHIFT, LINEAR_MAXIMUM_LOGIT_SHIFT);
            LINEAR_SCORE_BAND * linear.confidence * logit
        });
        AdaptiveScore {
            zero_shot_similarity,
            adapted_similarity: (zero_shot_similarity + cache_shift + linear_shift)
                .clamp(-1.0, 1.0),
        }
    }
}

fn cache_vote(image: &[f32], feedback: &[FeedbackExample]) -> f32 {
    let mut positive = Vec::new();
    let mut negative = Vec::new();
    for example in feedback {
        let affinity = dot(image, &example.values).clamp(-1.0, 1.0);
        let support = (CACHE_BETA * (affinity - 1.0)).exp() * example.weight;
        match example.label {
            FeedbackLabel::Belongs => positive.push(support),
            FeedbackLabel::DoesNotBelong => negative.push(support),
        }
    }
    (top_support(&mut positive) - top_support(&mut negative)).clamp(-1.0, 1.0)
}

fn top_support(values: &mut [f32]) -> f32 {
    values.sort_by(|left, right| right.total_cmp(left));
    let selected = values.iter().take(3);
    let (sum, count) = selected.fold((0.0, 0usize), |(sum, count), value| {
        (sum + value, count + 1)
    });
    if count == 0 { 0.0 } else { sum / count as f32 }
}

fn train_linear_residual(
    prompt: &[f32],
    threshold: f32,
    feedback: &[FeedbackExample],
) -> Option<LinearResidual> {
    let positives = feedback
        .iter()
        .filter(|example| example.label == FeedbackLabel::Belongs)
        .count();
    let negatives = feedback
        .iter()
        .filter(|example| example.label == FeedbackLabel::DoesNotBelong)
        .count();
    if positives < MINIMUM_LINEAR_EXAMPLES_PER_CLASS
        || negatives < MINIMUM_LINEAR_EXAMPLES_PER_CLASS
    {
        return None;
    }

    let mut weights = vec![0.0; prompt.len()];
    let mut bias = 0.0;
    let positive_scale = 0.5 / positives as f32;
    let negative_scale = 0.5 / negatives as f32;
    for _ in 0..LINEAR_ITERATIONS {
        let mut gradient = vec![0.0; prompt.len()];
        let mut bias_gradient = 0.0;
        for example in feedback {
            let base_logit = (dot(prompt, &example.values) - threshold) / LINEAR_SCORE_BAND;
            let logit = base_logit + dot(&weights, &example.values) + bias;
            let probability = sigmoid(logit);
            let class_scale = if example.label == FeedbackLabel::Belongs {
                positive_scale
            } else {
                negative_scale
            };
            let error = (probability - example.label.target()) * class_scale * example.weight;
            for (gradient_value, feature) in gradient.iter_mut().zip(&example.values) {
                *gradient_value += error * feature;
            }
            bias_gradient += error;
        }
        for (weight, gradient_value) in weights.iter_mut().zip(gradient) {
            let regularized = gradient_value + LINEAR_REGULARIZATION * *weight;
            *weight -= LINEAR_LEARNING_RATE * regularized;
        }
        bias -= LINEAR_LEARNING_RATE * bias_gradient;
    }
    let evidence_count = positives.saturating_add(negatives) as f32;
    Some(LinearResidual {
        weights,
        bias,
        confidence: evidence_count / (evidence_count + 12.0),
    })
}

fn dot(left: &[f32], right: &[f32]) -> f32 {
    debug_assert_eq!(left.len(), right.len());
    left.iter()
        .zip(right)
        .map(|(left, right)| left * right)
        .sum()
}

fn sigmoid(value: f32) -> f32 {
    if value >= 0.0 {
        1.0 / (1.0 + (-value).exp())
    } else {
        let exponential = value.exp();
        exponential / (1.0 + exponential)
    }
}

#[cfg(test)]
mod tests;
