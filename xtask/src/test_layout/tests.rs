use std::{
    collections::{BTreeMap, BTreeSet},
    path::Path,
};

use syn::File;

use super::audit::{
    has_legacy_test_suffix, inline_test_identities_in_syntax, inline_test_modules_in_syntax,
    is_canonical_test_source, is_test_tree_facade, owner_test_path_override_findings_in_syntax,
    permanently_disabled_test_findings_in_syntax, source_inclusion_findings_in_syntax,
    test_facade_non_registration_items_in_syntax,
};
use super::*;

fn parse(source: &str) -> File {
    syn::parse_file(source).expect("parse Rust test fixture")
}

#[test]
fn inline_test_scan_records_module_qualified_executable_identities() {
    let syntax = parse(
        r#"
        /// ```
        /// assert_eq!(2 + 2, 4);
        /// ```
        const _: () = ();

        #[cfg(test)]
        mod tests {
            #[test]
            fn ordinary() {}

            #[tokio::test]
            async fn async_test() {}

            fn helper() {}
        }

        #[cfg(unix)]
        #[test]
        fn conditional_duplicate() {}

        #[cfg(windows)]
        #[test]
        fn conditional_duplicate() {}

        #[cfg_attr(feature = "conditional", test)]
        fn conditional_attribute() {}

        #[cfg(unix)]
        mod conditional_module {
            #[test]
            fn same_name() {}
        }

        #[cfg(windows)]
        mod conditional_module {
            #[test]
            fn same_name() {}
        }

        #[cfg(all(test, unix))]
        mod platform_tests {
            fn helper() {}
        }

        #[cfg(not(test))]
        mod production_only {
            fn helper() {}
        }
        "#,
    );

    let tests = inline_test_identities_in_syntax(&syntax);
    assert_eq!(tests.len(), 7);
    assert!(
        tests
            .iter()
            .any(|identity| identity.ends_with("::async_test"))
    );
    assert!(
        tests
            .iter()
            .any(|identity| identity.ends_with("::ordinary"))
    );
    assert_eq!(
        tests
            .iter()
            .filter(|identity| identity.starts_with("conditional_duplicate#[cfg("))
            .count(),
        2
    );
    assert!(
        tests
            .iter()
            .any(|identity| identity.starts_with("conditional_attribute#[cfg_attr("))
    );
    assert_eq!(
        tests
            .iter()
            .filter(|identity| {
                identity.starts_with("conditional_module#[cfg(")
                    && identity.ends_with("::same_name")
            })
            .count(),
        2
    );

    let modules = inline_test_modules_in_syntax(&syntax);
    assert_eq!(modules.len(), 2);
    assert!(
        modules
            .iter()
            .any(|identity| identity.starts_with("tests#[cfg(test"))
    );
    assert!(
        modules
            .iter()
            .any(|identity| identity.starts_with("platform_tests#[cfg(all"))
    );
    assert!(
        modules
            .iter()
            .all(|identity| !identity.starts_with("production_only"))
    );
}

#[test]
fn canonical_owner_test_paths_are_distinct_from_legacy_siblings() {
    assert!(is_canonical_test_source(Path::new("owner/tests.rs")));
    assert!(is_canonical_test_source(Path::new("owner/tests/cache.rs")));
    assert!(is_canonical_test_source(Path::new(
        "tests/facade_contract.rs"
    )));
    assert!(!is_canonical_test_source(Path::new("owner.rs")));
    assert!(has_legacy_test_suffix(Path::new("owner_tests.rs")));
    assert!(has_legacy_test_suffix(Path::new("owner_test.rs")));
    assert!(is_test_tree_facade(Path::new("owner/tests/mod.rs")));
    assert!(!is_test_tree_facade(Path::new("owner/tests/cache.rs")));
    assert!(!is_test_tree_facade(Path::new("owner/mod.rs")));
}

#[test]
fn test_tree_facade_accepts_only_out_of_line_module_registrations() {
    let registration_only = parse(
        r"
        //! Navigation for responsibility-owned contracts.
        mod cache_contract;
        #[cfg(unix)]
        mod platform_contract;
        ",
    );
    assert!(test_facade_non_registration_items_in_syntax(&registration_only).is_empty());

    let hidden_implementation = parse(
        r"
        use super::*;
        mod cache_contract;

        const FIXTURE: u8 = 1;

        #[test]
        fn facade_test() {}

        mod inline_support {
            pub(super) fn fixture() {}
        }
        ",
    );
    assert_eq!(
        test_facade_non_registration_items_in_syntax(&hidden_implementation),
        BTreeSet::from([
            "const FIXTURE".to_owned(),
            "fn facade_test".to_owned(),
            "inline mod inline_support".to_owned(),
            "use declaration".to_owned(),
        ])
    );
}

#[test]
fn crate_test_scan_rejects_private_source_inclusion_only() {
    let syntax = parse(
        r#"
        #[path = "../src/private_protocol.rs"]
        mod private_protocol;
        #[cfg_attr(feature = "conditional", path = "../src/conditional_protocol.rs")]
        mod conditional_protocol;
        include!("../src/generated_contract.rs");
        const NOTICE: &str = include_str!("notice.txt");
        mod fixture;
        "#,
    );

    assert_eq!(
        source_inclusion_findings_in_syntax(&syntax),
        BTreeSet::from([
            "include!:\"../src/generated_contract.rs\"".to_owned(),
            "path:../src/conditional_protocol.rs".to_owned(),
            "path:../src/private_protocol.rs".to_owned(),
        ])
    );
}

#[test]
fn production_owner_scan_rejects_redirected_test_modules_only() {
    let syntax = parse(
        r#"
        #[cfg(test)]
        #[path = "shared_tests/owner.rs"]
        mod tests;

        #[cfg_attr(feature = "alternate-tests", path = "alternate/tests.rs")]
        mod tests;

        #[path = "generated/product.rs"]
        mod generated_product;
        "#,
    );

    assert_eq!(
        owner_test_path_override_findings_in_syntax(&syntax),
        BTreeSet::from([
            "path:alternate/tests.rs".to_owned(),
            "path:shared_tests/owner.rs".to_owned(),
        ])
    );
}

#[test]
fn canonical_test_scan_rejects_constant_false_cfg_but_allows_runnable_ignores() {
    let syntax = parse(
        r#"
        #[cfg(any())]
        fn hidden_helper() {}

        #[cfg(all(unix, any()))]
        #[test]
        fn hidden_test() {}

        #[cfg_attr(all(), cfg(any()))]
        fn cfg_attr_hidden_helper() {}

        #[cfg(not(any()))]
        fn always_present_helper() {}

        #[cfg(any(unix, windows))]
        #[test]
        fn maintained_platform_test() {}

        #[test]
        #[ignore = "requires SHADOW_TEST_RAW_FOLDER with one decodable RAW"]
        fn explicit_external_fixture_contract() {}
        "#,
    );

    let findings = permanently_disabled_test_findings_in_syntax(&syntax, true);
    assert_eq!(findings.len(), 3);
    assert!(findings.iter().any(|finding| finding == "#[cfg(any ())]"));
    assert!(
        findings
            .iter()
            .any(|finding| finding.starts_with("#[cfg(all"))
    );
    assert!(
        findings
            .iter()
            .any(|finding| finding.starts_with("#[cfg_attr(all"))
    );
}

#[test]
fn production_owner_scan_rejects_only_a_disabled_canonical_test_registration() {
    let syntax = parse(
        r"
        #[cfg(any())]
        mod tests;

        #[cfg(any())]
        fn unrelated_dead_production_prototype() {}
        ",
    );

    assert_eq!(
        permanently_disabled_test_findings_in_syntax(&syntax, false),
        BTreeSet::from(["mod tests#[cfg(any ())]".to_owned()])
    );
}

#[test]
fn strict_policy_reports_every_present_violation_category() {
    let observation = TestLayoutObservation {
        inline_test_modules: BTreeMap::from([(
            "src/owner.rs".to_owned(),
            BTreeSet::from(["tests#[cfg(test)]".to_owned()]),
        )]),
        inline_executable_tests: BTreeMap::from([(
            "src/owner.rs".to_owned(),
            BTreeSet::from(["tests::ordinary".to_owned()]),
        )]),
        owner_test_path_overrides: BTreeMap::from([(
            "src/owner.rs".to_owned(),
            BTreeSet::from(["path:shared/tests.rs".to_owned()]),
        )]),
        crate_test_source_inclusions: BTreeMap::from([(
            "tests/public.rs".to_owned(),
            BTreeSet::from(["path:../src/private.rs".to_owned()]),
        )]),
        permanently_disabled_test_sources: BTreeMap::from([(
            "src/owner/tests.rs".to_owned(),
            BTreeSet::from(["#[cfg(any ())]".to_owned()]),
        )]),
        test_facade_non_registration_items: BTreeMap::from([(
            "src/owner/tests/mod.rs".to_owned(),
            BTreeSet::from(["use declaration".to_owned()]),
        )]),
        sibling_test_sources: BTreeSet::from(["src/owner_tests.rs".to_owned()]),
    };

    let error = policy::enforce(&observation).expect_err("every violation must fail");
    let message = error.to_string();
    for category in [
        "Sibling *_test.rs",
        "Inline #[cfg(test)]",
        "Executable test bodies",
        "Owner tests redirected",
        "Private production source inclusions",
        "Permanently disabled",
        "Implementation hidden",
    ] {
        assert!(message.contains(category), "missing category: {category}");
    }
}

#[test]
fn strict_policy_accepts_only_a_zero_observation() {
    policy::enforce(&TestLayoutObservation::default()).expect("zero topology is accepted");
}

#[cfg(unix)]
#[test]
fn source_discovery_fails_closed_on_symlinks() {
    use std::{fs, os::unix::fs::symlink, time::SystemTime};

    let unique = SystemTime::now()
        .duration_since(SystemTime::UNIX_EPOCH)
        .expect("test clock is after the Unix epoch")
        .as_nanos();
    let root = std::env::temp_dir().join(format!(
        "shadow-test-layout-symlink-{}-{unique}",
        std::process::id()
    ));
    fs::create_dir_all(&root).expect("create symlink audit fixture");
    fs::write(root.join("owner.rs"), "pub fn owner() {}").expect("write fixture source");
    symlink(root.join("owner.rs"), root.join("linked.rs")).expect("create fixture symlink");

    let error = super::audit::rust_files_below(&root).expect_err("symlink must fail closed");
    assert!(error.to_string().contains("symlinked source path"));

    fs::remove_dir_all(&root).expect("remove symlink audit fixture");
}
