use super::super::claims::ClaimFile;

pub(super) fn claim(scope: &str, dependencies: &[&str]) -> ClaimFile {
    ClaimFile {
        schema: Some(1),
        scope: scope.to_owned(),
        owner: "test".to_owned(),
        task: Some("focused test task".to_owned()),
        paths: Vec::new(),
        created_at: Some("2026-07-26T00:00:00Z".to_owned()),
        heartbeat_at: Some("2026-07-26T00:00:00Z".to_owned()),
        first_release: None,
        depends_on: dependencies
            .iter()
            .map(|value| (*value).to_owned())
            .collect(),
        status: None,
        kind: None,
        transaction_steward: None,
        participants: Vec::new(),
        resume_condition: None,
        checkpoint: None,
        pause_retained_paths_reason: None,
        overlap_reason: None,
    }
}
