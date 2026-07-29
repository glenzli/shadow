use std::sync::Mutex;

use crate::{
    AdmittedExecution, AdmittedModelIdentity, AiCapability, AiObservation,
    AiObservationContractError, CancellationToken, ExecutionLease, ExecutionPlanIdentity,
    ExecutionRouteIdentity, ExplanationSignal, MAX_EXPLANATION_SIGNALS, ProposalReviewLevel,
    ProviderTerminal, RUNTIME_PROGRESS_COMPLETE, RuntimeFailure, RuntimeProgress,
    RuntimeProgressError, RuntimeProgressReporter, RuntimeProgressSink, RuntimeProvider,
    RuntimeTerminalOutcome, RuntimeUsage, UnitInterval,
};

use super::fixtures::admitted_execution;

#[derive(Debug)]
struct SuccessfulProvider {
    route: ExecutionRouteIdentity,
    execution_plan: ExecutionPlanIdentity,
}

impl SuccessfulProvider {
    fn for_execution(execution: &AdmittedExecution) -> Self {
        Self {
            route: execution.route().clone(),
            execution_plan: execution.plan_identity().clone(),
        }
    }
}

impl RuntimeProvider for SuccessfulProvider {
    type Output = u32;

    fn route(&self) -> &ExecutionRouteIdentity {
        &self.route
    }

    fn execution_plan(&self) -> &ExecutionPlanIdentity {
        &self.execution_plan
    }

    fn supports(&self, capability: AiCapability) -> bool {
        capability == AiCapability::SimilarityEmbedding
    }

    fn execute(
        &self,
        _execution: &AdmittedExecution,
        _cancellation: &CancellationToken,
        progress: &RuntimeProgressReporter<'_>,
    ) -> ProviderTerminal<Self::Output> {
        progress
            .publish(RuntimeProgress {
                phase_code: "feature_print".into(),
                completed_basis_points: RUNTIME_PROGRESS_COMPLETE,
            })
            .expect("monotonic progress");
        ProviderTerminal::Succeeded {
            output: 42,
            usage: RuntimeUsage {
                elapsed_ms: 5,
                ..RuntimeUsage::default()
            },
        }
    }
}

#[derive(Debug, Default)]
struct RecordingProgress {
    events: Mutex<Vec<RuntimeProgress>>,
}

impl RuntimeProgressSink for RecordingProgress {
    fn publish(&self, progress: RuntimeProgress) {
        self.events.lock().expect("progress lock").push(progress);
    }
}

fn completed_observation() -> AiObservation<u32> {
    let execution = admitted_execution();
    let provider = SuccessfulProvider::for_execution(&execution);
    let receipt = ExecutionLease::issue("observation-lease".into(), execution)
        .expect("lease")
        .execute(&provider, &CancellationToken::default());
    let RuntimeTerminalOutcome::Succeeded { output } = receipt.outcome else {
        panic!("expected completed provider output");
    };
    (*output)
        .into_observation(
            UnitInterval::new(0.8).expect("confidence"),
            ProposalReviewLevel::NeedsConfirmation,
            vec![ExplanationSignal {
                code: "visual_similarity".into(),
                value: 0.8,
                detail: None,
            }],
        )
        .expect("runtime-bound observation")
}

#[test]
fn a_consumed_lease_emits_one_identity_bound_terminal_receipt() {
    let execution = admitted_execution();
    let expected_route = execution.route().clone();
    let expected_plan = execution.plan_identity().clone();
    let provider = SuccessfulProvider::for_execution(&execution);
    let receipt = ExecutionLease::issue("lease-1".into(), execution)
        .expect("lease")
        .execute(&provider, &CancellationToken::default());

    assert_eq!(receipt.execution_id, "execution-1");
    assert_eq!(receipt.route, expected_route);
    assert_eq!(receipt.execution_plan, expected_plan);
    let RuntimeTerminalOutcome::Succeeded { output } = &receipt.outcome else {
        panic!("expected a lease-bound successful output");
    };
    assert_eq!(*output.payload(), 42);
    assert_eq!(
        output.provenance().request_id(),
        receipt.request_id.as_str()
    );
    assert_eq!(output.provenance().generation(), receipt.generation);
    assert_eq!(output.provenance().execution_route(), &receipt.route);
    assert_eq!(
        output.provenance().execution_plan(),
        &receipt.execution_plan
    );
    output
        .provenance()
        .validate()
        .expect("runtime-issued provenance");
}

#[test]
fn successful_lease_output_constructs_the_complete_observation_identity() {
    let observation = completed_observation();

    assert_eq!(
        observation.request_id(),
        observation.provenance().request_id()
    );
    assert_eq!(
        observation.generation(),
        observation.provenance().generation()
    );
    assert_eq!(observation.task(), observation.provenance().task());
    assert_eq!(observation.target(), observation.provenance().target());
    assert_eq!(*observation.payload(), 42);
    observation.validate().expect("exact runtime observation");

    let encoded = serde_json::to_vec(&observation).expect("serialize observation");
    let decoded: AiObservation<u32> =
        serde_json::from_slice(&encoded).expect("decode exact observation");
    assert_eq!(decoded, observation);
}

#[test]
fn observation_decoder_rejects_unknown_version_fields_and_oversized_signals() {
    let observation = completed_observation();
    let value = serde_json::to_value(&observation).expect("serialize observation");

    let mut unsupported = value.clone();
    unsupported["contract_version"] = serde_json::json!(crate::AI_OBSERVATION_CONTRACT_VERSION + 1);
    assert!(serde_json::from_value::<AiObservation<u32>>(unsupported).is_err());

    let mut unknown = value.clone();
    unknown["provider_response"] = serde_json::json!("untrusted");
    assert!(serde_json::from_value::<AiObservation<u32>>(unknown).is_err());

    let mut unknown_signal = value.clone();
    unknown_signal["explanation_signals"][0]["provider_weight"] = serde_json::json!(0.5);
    assert!(serde_json::from_value::<AiObservation<u32>>(unknown_signal).is_err());

    let mut oversized = value;
    oversized["explanation_signals"] = serde_json::Value::Array(vec![
        serde_json::json!({
            "code": "signal",
            "value": 1.0,
            "detail": null
        });
        MAX_EXPLANATION_SIGNALS + 1
    ]);
    assert!(serde_json::from_value::<AiObservation<u32>>(oversized).is_err());
}

#[test]
fn observation_decoder_rejects_every_provenance_identity_mismatch() {
    let observation = completed_observation();
    let value = serde_json::to_value(&observation).expect("serialize observation");

    let mut mismatches = Vec::new();
    let mut request_id = value.clone();
    request_id["request_id"] = serde_json::json!("substituted");
    mismatches.push(request_id);
    let mut generation = value.clone();
    generation["generation"] = serde_json::json!(observation.generation() + 1);
    mismatches.push(generation);
    let mut task = value.clone();
    task["task"] = serde_json::json!("generate_caption");
    mismatches.push(task);
    let mut target = value;
    target["target"] = serde_json::json!({"kind": "library"});
    mismatches.push(target);

    for mismatch in mismatches {
        assert!(serde_json::from_value::<AiObservation<u32>>(mismatch).is_err());
    }
}

#[test]
fn observation_construction_rejects_an_oversized_signal_inventory() {
    let execution = admitted_execution();
    let provider = SuccessfulProvider::for_execution(&execution);
    let receipt = ExecutionLease::issue("oversized-observation-lease".into(), execution)
        .expect("lease")
        .execute(&provider, &CancellationToken::default());
    let RuntimeTerminalOutcome::Succeeded { output } = receipt.outcome else {
        panic!("expected completed provider output");
    };
    let signals = vec![
        ExplanationSignal {
            code: "signal".into(),
            value: 1.0,
            detail: None,
        };
        MAX_EXPLANATION_SIGNALS + 1
    ];
    assert!(matches!(
        (*output).into_observation(
            UnitInterval::ONE,
            ProposalReviewLevel::ObservationOnly,
            signals,
        ),
        Err(AiObservationContractError::TooManyExplanationSignals(
            count
        )) if count == MAX_EXPLANATION_SIGNALS + 1
    ));
}

#[test]
fn observation_construction_rejects_malformed_explanation_signals() {
    for (signal, expected) in [
        (
            ExplanationSignal {
                code: " ".into(),
                value: 1.0,
                detail: None,
            },
            AiObservationContractError::MissingExplanationCode(0),
        ),
        (
            ExplanationSignal {
                code: "signal".into(),
                value: f64::NAN,
                detail: None,
            },
            AiObservationContractError::NonFiniteExplanationValue(0),
        ),
    ] {
        let execution = admitted_execution();
        let provider = SuccessfulProvider::for_execution(&execution);
        let receipt = ExecutionLease::issue("invalid-signal-lease".into(), execution)
            .expect("lease")
            .execute(&provider, &CancellationToken::default());
        let RuntimeTerminalOutcome::Succeeded { output } = receipt.outcome else {
            panic!("expected completed provider output");
        };

        assert_eq!(
            (*output).into_observation(
                UnitInterval::ONE,
                ProposalReviewLevel::ObservationOnly,
                vec![signal],
            ),
            Err(expected)
        );
    }
}

#[test]
fn provider_progress_reaches_the_sink_through_a_monotonic_contract() {
    let execution = admitted_execution();
    let provider = SuccessfulProvider::for_execution(&execution);
    let progress = RecordingProgress::default();
    let receipt = ExecutionLease::issue("lease-1".into(), execution)
        .expect("lease")
        .execute_with_progress(&provider, &CancellationToken::default(), &progress);

    assert!(matches!(
        receipt.outcome,
        RuntimeTerminalOutcome::Succeeded { .. }
    ));
    assert_eq!(
        progress.events.lock().expect("progress lock").as_slice(),
        &[RuntimeProgress {
            phase_code: "feature_print".into(),
            completed_basis_points: RUNTIME_PROGRESS_COMPLETE,
        }]
    );
}

#[test]
fn progress_reporter_rejects_regression() {
    let progress = RecordingProgress::default();
    let reporter = RuntimeProgressReporter::new(&progress);
    reporter
        .publish(RuntimeProgress {
            phase_code: "extract".into(),
            completed_basis_points: 5000,
        })
        .expect("first progress");
    assert_eq!(
        reporter.publish(RuntimeProgress {
            phase_code: "compare".into(),
            completed_basis_points: 4999,
        }),
        Err(RuntimeProgressError::Regression {
            previous: 5000,
            next: 4999,
        })
    );
}

#[test]
fn cancellation_before_execution_never_invokes_or_fakes_provider_output() {
    let execution = admitted_execution();
    let provider = SuccessfulProvider::for_execution(&execution);
    let cancellation = CancellationToken::default();
    cancellation.cancel();
    let receipt = ExecutionLease::issue("lease-1".into(), execution)
        .expect("lease")
        .execute(&provider, &cancellation);

    assert!(matches!(receipt.outcome, RuntimeTerminalOutcome::Cancelled));
    assert_eq!(receipt.usage, RuntimeUsage::default());
}

#[test]
fn provider_must_match_the_complete_model_route_not_only_adapter_identity() {
    let execution = admitted_execution();
    let mut mismatched_route = execution.route().clone();
    let AdmittedModelIdentity::LocalArtifactSet { exact_revision, .. } =
        &mut mismatched_route.model
    else {
        panic!("local artifact route");
    };
    *exact_revision = "another-revision".into();
    let execution_plan = execution.plan_identity().clone();
    let provider = SuccessfulProvider {
        route: mismatched_route,
        execution_plan,
    };
    let receipt = ExecutionLease::issue("lease-route-mismatch".into(), execution)
        .expect("lease")
        .execute(&provider, &CancellationToken::default());

    assert!(matches!(
        receipt.outcome,
        RuntimeTerminalOutcome::Failed {
            failure: RuntimeFailure::ExecutionRouteMismatch,
        }
    ));
}

#[test]
fn provider_must_match_the_complete_execution_plan_even_on_the_same_route() {
    let execution = admitted_execution();
    let mut different_plan = execution.plan().clone();
    different_plan.reserved_system_ram_bytes += 1;
    let provider = SuccessfulProvider {
        route: execution.route().clone(),
        execution_plan: ExecutionPlanIdentity::from_plan(different_plan)
            .expect("valid different plan identity"),
    };
    let receipt = ExecutionLease::issue("lease-plan-mismatch".into(), execution)
        .expect("lease")
        .execute(&provider, &CancellationToken::default());

    assert!(matches!(
        receipt.outcome,
        RuntimeTerminalOutcome::Failed {
            failure: RuntimeFailure::ExecutionPlanMismatch,
        }
    ));
}
