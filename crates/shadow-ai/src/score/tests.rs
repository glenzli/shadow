use super::*;

fn unit(value: f64) -> UnitInterval {
    UnitInterval::new(value).expect("valid unit value")
}

fn signal(value: f64, confidence: f64) -> ScoredSignal {
    ScoredSignal {
        value: unit(value),
        confidence: unit(confidence),
    }
}

fn policy() -> SelectionPolicy {
    SelectionPolicy {
        weights: SelectionWeights {
            technical_quality: 0.5,
            general_prior: 0.2,
            personal_preference: 0.25,
            uniqueness: 0.05,
        },
        hard_defect_threshold: unit(0.8),
        hard_defect_minimum_confidence: unit(0.8),
        unique_moment_threshold: unit(0.85),
        duplicate_collapse_threshold: unit(0.95),
        minimum_personal_evidence: 100,
        confirmation_threshold: unit(0.6),
        proposal_threshold: unit(0.85),
    }
}

fn candidate(id: &str, quality: f64) -> CandidateAssessment {
    CandidateAssessment {
        candidate_id: id.into(),
        signals: CandidateSignals {
            hard_defect: signal(0.1, 0.9),
            technical_quality: signal(quality, 0.9),
            general_prior: signal(0.5, 0.7),
            personal_preference: None,
            uniqueness: signal(0.4, 0.8),
            duplicate_similarity: None,
        },
        manually_protected: false,
    }
}

#[test]
fn a_unique_moment_is_never_excluded_by_the_defect_gate() {
    let mut unique = candidate("unique", 0.2);
    unique.signals.hard_defect = signal(0.95, 0.99);
    unique.signals.uniqueness = signal(0.95, 0.9);
    let ranked = rank_group(&[unique], policy()).expect("rank candidate");
    assert_eq!(ranked[0].defect_gate, DefectGate::ManualReview);
    assert_eq!(ranked[0].visibility, VisibilityDisposition::AlwaysVisible);
}

#[test]
fn cold_personal_signal_cannot_overpower_technical_quality() {
    let strong = candidate("sharp", 0.9);
    let mut cold = candidate("cold-preference", 0.4);
    cold.signals.personal_preference = Some(PersonalPreferenceSignal {
        score: UnitInterval::ONE,
        confidence: UnitInterval::ONE,
        explicit_evidence_count: 1,
    });
    let ranked = rank_group(&[cold, strong], policy()).expect("rank group");
    assert_eq!(ranked[0].candidate_id, "sharp");
}

#[test]
fn ties_are_stable_by_candidate_identity() {
    let ranked =
        rank_group(&[candidate("b", 0.5), candidate("a", 0.5)], policy()).expect("rank group");
    assert_eq!(ranked[0].candidate_id, "a");
    assert_eq!(ranked[1].candidate_id, "b");
}
