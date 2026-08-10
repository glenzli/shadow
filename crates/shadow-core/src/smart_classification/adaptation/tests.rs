use super::{AdaptiveCategoryModel, FeedbackExample, FeedbackLabel};

#[test]
fn local_feedback_moves_similar_photos_without_replacing_the_prompt_prior() {
    let prompt = [1.0, 0.0, 0.0];
    let positive = FeedbackExample {
        values: vec![0.80, 0.60, 0.0],
        label: FeedbackLabel::Belongs,
        weight: 1.0,
    };
    let negative = FeedbackExample {
        values: vec![0.80, -0.60, 0.0],
        label: FeedbackLabel::DoesNotBelong,
        weight: 1.0,
    };
    let positive_model = AdaptiveCategoryModel::new(&prompt, 0.79, vec![positive]);
    let negative_model = AdaptiveCategoryModel::new(&prompt, 0.79, vec![negative]);
    let candidate = [0.80, 0.60, 0.0];

    let promoted = positive_model.score(&candidate);
    let demoted = negative_model.score(&candidate);
    assert_eq!(promoted.zero_shot_similarity, 0.80);
    assert!(promoted.adapted_similarity > promoted.zero_shot_similarity);
    assert!(demoted.adapted_similarity < demoted.zero_shot_similarity);
}

#[test]
fn linear_residual_requires_balanced_evidence_and_generalizes_its_direction() {
    let prompt = [1.0, 0.0];
    let mut examples = Vec::new();
    for offset in [0.0, 0.02, -0.02, 0.01] {
        examples.push(FeedbackExample {
            values: vec![0.79 + offset, 0.61],
            label: FeedbackLabel::Belongs,
            weight: 1.0,
        });
        examples.push(FeedbackExample {
            values: vec![0.79 + offset, -0.61],
            label: FeedbackLabel::DoesNotBelong,
            weight: 1.0,
        });
    }
    let model = AdaptiveCategoryModel::new(&prompt, 0.79, examples);
    let positive = model.score(&[0.79, 0.55]);
    let negative = model.score(&[0.79, -0.55]);
    assert!(positive.adapted_similarity > negative.adapted_similarity);
}
