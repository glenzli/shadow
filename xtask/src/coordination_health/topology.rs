use super::claims::{ClaimReport, HealthReport};
use std::{
    collections::{BTreeMap, BTreeSet},
    path::Path,
};

pub(super) fn annotate_dependency_findings(report: &mut HealthReport) {
    let known_scopes = report
        .claims
        .iter()
        .map(|report| report.claim.scope.clone())
        .collect::<BTreeSet<_>>();
    for claim_report in &mut report.claims {
        for dependency in &claim_report.claim.depends_on {
            if !known_scopes.contains(dependency) {
                claim_report.findings.push(format!(
                    "depends_on references inactive or missing scope {dependency:?}",
                ));
            }
        }
    }
    for cycle in &report.cycles {
        if let Some(scope) = cycle.first()
            && let Some(claim_report) = report
                .claims
                .iter_mut()
                .find(|report| &report.claim.scope == scope)
        {
            claim_report
                .findings
                .push(format!("dependency cycle detected: {}", cycle.join(" -> ")));
        }
    }
}

pub(super) fn annotate_path_overlaps(report: &mut HealthReport) {
    let mut findings = Vec::<(usize, String)>::new();
    for left_index in 0..report.claims.len() {
        for right_index in left_index + 1..report.claims.len() {
            let left = &report.claims[left_index].claim;
            let right = &report.claims[right_index].claim;
            let overlaps = overlapping_paths(&left.paths, &right.paths);
            if overlaps.is_empty() {
                continue;
            }
            let left_explained = left
                .overlap_reason
                .as_deref()
                .is_some_and(|value| !value.trim().is_empty());
            let right_explained = right
                .overlap_reason
                .as_deref()
                .is_some_and(|value| !value.trim().is_empty());
            let explanation = if left_explained || right_explained {
                "explicitly explained overlap"
            } else {
                "unexplained overlap"
            };
            let paths = overlaps.join(", ");
            findings.push((
                left_index,
                format!("{explanation} with scope {:?}: {paths}", right.scope),
            ));
            findings.push((
                right_index,
                format!("{explanation} with scope {:?}: {paths}", left.scope),
            ));
        }
    }
    for (index, finding) in findings {
        report.claims[index].findings.push(finding);
    }
}

fn overlapping_paths(left: &[String], right: &[String]) -> Vec<String> {
    let mut overlaps = BTreeSet::new();
    for left_path in left {
        for right_path in right {
            if paths_overlap(left_path, right_path) {
                overlaps.insert(format!("{left_path} ↔ {right_path}"));
            }
        }
    }
    overlaps.into_iter().collect()
}

pub(super) fn paths_overlap(left: &str, right: &str) -> bool {
    let left = Path::new(left);
    let right = Path::new(right);
    left == right || left.starts_with(right) || right.starts_with(left)
}

pub(super) fn dependency_cycles(claims: &[ClaimReport]) -> Vec<Vec<String>> {
    let adjacency = claims
        .iter()
        .map(|report| (report.claim.scope.clone(), report.claim.depends_on.clone()))
        .collect::<BTreeMap<_, _>>();
    let mut states = BTreeMap::<String, VisitState>::new();
    let mut stack = Vec::new();
    let mut seen_cycles = BTreeSet::new();
    let mut cycles = Vec::new();
    for scope in adjacency.keys() {
        visit_dependency(
            scope,
            &adjacency,
            &mut states,
            &mut stack,
            &mut seen_cycles,
            &mut cycles,
        );
    }
    cycles
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum VisitState {
    Visiting,
    Complete,
}

fn visit_dependency(
    scope: &str,
    adjacency: &BTreeMap<String, Vec<String>>,
    states: &mut BTreeMap<String, VisitState>,
    stack: &mut Vec<String>,
    seen_cycles: &mut BTreeSet<String>,
    cycles: &mut Vec<Vec<String>>,
) {
    if matches!(states.get(scope), Some(VisitState::Complete)) {
        return;
    }
    if matches!(states.get(scope), Some(VisitState::Visiting)) {
        let position = stack.iter().position(|candidate| candidate == scope);
        if let Some(position) = position {
            let mut cycle = stack[position..].to_vec();
            cycle.push(scope.to_owned());
            let key = cycle.join("\u{1f}");
            if seen_cycles.insert(key) {
                cycles.push(cycle);
            }
        }
        return;
    }

    states.insert(scope.to_owned(), VisitState::Visiting);
    stack.push(scope.to_owned());
    if let Some(dependencies) = adjacency.get(scope) {
        for dependency in dependencies {
            if adjacency.contains_key(dependency) {
                visit_dependency(dependency, adjacency, states, stack, seen_cycles, cycles);
            }
        }
    }
    stack.pop();
    states.insert(scope.to_owned(), VisitState::Complete);
}
