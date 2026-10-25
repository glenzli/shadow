use super::*;

fn torch_cuda_provenance() -> VisionProvenance {
    VisionProvenance {
        job_id: "sam-cuda-fixture".into(),
        provider: "torch-sam-local".into(),
        deployment: "sam21_small_torch_cuda_fp32_v1".into(),
        model_build: "sam21_small_torch_cuda_fp32_v1".into(),
        artifact_sha256: "36c9535ba0a7bd7240fac01563b97c22e1a5230d5f07cd7ba8c69a49c8cbf119".into(),
        preprocessing_identity: "sam21_transformers_exact1024_rgb_imagenet_v1".into(),
        postprocessing_identity: "sam21_torch_best_iou_native256_sigmoid_binary_bilinear_v1".into(),
        tokenizer: None,
        runtime: "transformers-4.56.2-torch-2.8.0-cu128".into(),
        requested_execution_provider: "cuda".into(),
        actual_execution_provider: "cuda".into(),
        execution_provider_fallback_reason: None,
        precision: "float32".into(),
    }
}

#[test]
fn cuda_promotion_keeps_the_actual_backend_and_independent_model_build_identity() {
    let provenance = torch_cuda_provenance();
    for capability in [
        shadow_ai::INFER_SUBJECT_MASK_CAPABILITY,
        shadow_ai::INFER_IMAGE_COMPLETION_CAPABILITY,
    ] {
        let (route, plan) = admit(&provenance, capability).unwrap();
        assert_eq!(plan.backend_kind, BackendKind::Cuda);
        assert_eq!(plan.precision, NumericPrecision::Float32);
        assert_eq!(route.provider.provider_id, provenance.provider);
        assert_eq!(
            route.provider.adapter_revision,
            provenance.postprocessing_identity
        );
        assert_eq!(
            route.provider.execution_class,
            ProviderExecutionClass::LocalService
        );
        let AdmittedModelIdentity::RemoteService {
            service_revision,
            model_id,
            model_revision,
            api_contract_revision,
        } = route.model
        else {
            panic!("Infer result lost its exact service identity");
        };
        assert_eq!(service_revision, provenance.runtime);
        assert_eq!(model_id, provenance.model_build);
        assert_eq!(model_revision, provenance.artifact_sha256);
        assert_eq!(api_contract_revision, capability);
    }
}

#[test]
fn legacy_cpu_and_coreml_plans_keep_their_backend_and_precision() {
    for (actual, backend) in [
        ("CPUExecutionProvider", BackendKind::Cpu),
        ("CoreMLExecutionProvider", BackendKind::CoreMl),
    ] {
        let mut provenance = torch_cuda_provenance();
        provenance.requested_execution_provider = actual.into();
        provenance.actual_execution_provider = actual.into();
        provenance.precision = "fp16".into();
        let (_, plan) = admit(&provenance, shadow_ai::INFER_SUBJECT_MASK_CAPABILITY).unwrap();
        assert_eq!(plan.backend_kind, backend);
        assert_eq!(plan.precision, NumericPrecision::Float16);
    }
}

#[test]
fn a_requested_cuda_result_cannot_silently_use_cpu_or_another_backend() {
    for actual in ["cpu", "CPUExecutionProvider", "coreml", "cuda+cpu"] {
        let mut provenance = torch_cuda_provenance();
        provenance.actual_execution_provider = actual.into();
        assert!(matches!(
            admit(&provenance, shadow_ai::INFER_SUBJECT_MASK_CAPABILITY),
            Err(AdmissionError::FallbackDisclosed)
        ));
    }
}

#[test]
fn disclosed_fallback_unknown_execution_and_mismatched_cuda_requests_are_rejected() {
    let mut provenance = torch_cuda_provenance();
    provenance.execution_provider_fallback_reason = Some("accelerator unavailable".into());
    assert!(matches!(
        admit(&provenance, shadow_ai::INFER_SUBJECT_MASK_CAPABILITY),
        Err(AdmissionError::FallbackDisclosed)
    ));
    provenance.execution_provider_fallback_reason = None;
    provenance.requested_execution_provider = "cpu".into();
    assert!(matches!(
        admit(&provenance, shadow_ai::INFER_SUBJECT_MASK_CAPABILITY),
        Err(AdmissionError::MalformedProvenance)
    ));
    provenance.requested_execution_provider = "cuda".into();
    provenance.precision = "unverified-precision".into();
    assert!(matches!(
        admit(&provenance, shadow_ai::INFER_SUBJECT_MASK_CAPABILITY),
        Err(AdmissionError::MalformedProvenance)
    ));
}
