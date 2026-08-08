use super::*;

#[test]
fn complete_upstream_identities_are_digest_bound_and_persistable() {
    let raw_plan = "raw-plan-field;".repeat(1_024);
    let helper_graph = "helper-graph-field;".repeat(1_024);

    let identity = grid_proxy_variant_key(2_048, 90, &raw_plan, Some(&helper_graph));

    assert_eq!(
        identity,
        format!(
            "shadow-photo-router:grid-jpeg-2048-q90-444-v3;raw-plan-b3={};\
             isolated-graph-b3={}",
            blake3::hash(raw_plan.as_bytes()).to_hex(),
            blake3::hash(helper_graph.as_bytes()).to_hex()
        )
    );
    assert!(
        identity.len() <= 256,
        "Review feedback must be able to persist every generated proxy identity"
    );
    assert!(!identity.contains("raw-plan-field"));
    assert!(!identity.contains("helper-graph-field"));
}

#[test]
fn every_rendering_input_changes_the_variant_identity() {
    let baseline = grid_proxy_variant_key(2_048, 90, "preview-plan", None);

    assert_ne!(
        baseline,
        grid_proxy_variant_key(4_096, 90, "preview-plan", None)
    );
    assert_ne!(
        baseline,
        grid_proxy_variant_key(2_048, 96, "preview-plan", None)
    );
    assert_ne!(
        baseline,
        grid_proxy_variant_key(2_048, 90, "detail-plan", None)
    );
    assert_ne!(
        baseline,
        grid_proxy_variant_key(2_048, 90, "preview-plan", Some("helper-a"))
    );
    assert_ne!(
        grid_proxy_variant_key(2_048, 90, "preview-plan", Some("helper-a")),
        grid_proxy_variant_key(2_048, 90, "preview-plan", Some("helper-b"))
    );
}
