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
