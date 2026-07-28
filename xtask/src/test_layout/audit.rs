use std::{
    collections::{BTreeMap, BTreeSet},
    env,
    ffi::OsString,
    fs, io,
    path::{Component, Path, PathBuf},
    process::Command,
};

use serde::Deserialize;
use syn::{
    Attribute, Expr, File, ImplItemFn, Item, ItemFn, ItemMod, Lit, Macro, Meta, Token,
    punctuated::Punctuated,
    visit::{self, Visit},
};

use super::invalid_data;

#[derive(Debug, Default, Eq, PartialEq)]
pub(super) struct TestLayoutObservation {
    pub(super) inline_test_modules: BTreeMap<String, BTreeSet<String>>,
    pub(super) inline_executable_tests: BTreeMap<String, BTreeSet<String>>,
    pub(super) crate_test_source_inclusions: BTreeMap<String, BTreeSet<String>>,
    pub(super) permanently_disabled_test_sources: BTreeMap<String, BTreeSet<String>>,
    pub(super) test_facade_non_registration_items: BTreeMap<String, BTreeSet<String>>,
    pub(super) sibling_test_sources: BTreeSet<String>,
}

#[derive(Debug, Deserialize)]
struct CargoMetadata {
    packages: Vec<CargoPackage>,
    workspace_members: Vec<String>,
}

#[derive(Debug, Deserialize)]
struct CargoPackage {
    id: String,
    manifest_path: PathBuf,
}

#[derive(Debug, Default)]
struct InlineTestVisitor {
    modules: BTreeSet<String>,
    module_path: Vec<String>,
    identities: BTreeSet<String>,
}

impl InlineTestVisitor {
    fn record(&mut self, name: &str, attributes: &[Attribute]) {
        let identity = if self.module_path.is_empty() {
            name.to_owned()
        } else {
            format!("{}::{name}", self.module_path.join("::"))
        };
        self.identities
            .insert(format!("{identity}{}", cfg_identity_suffix(attributes)));
    }
}

impl<'ast> Visit<'ast> for InlineTestVisitor {
    fn visit_item_mod(&mut self, item: &'ast ItemMod) {
        self.module_path.push(format!(
            "{}{}",
            item.ident,
            cfg_identity_suffix(&item.attrs)
        ));
        if item.content.is_some() && item.attrs.iter().any(attribute_is_test_cfg) {
            self.modules.insert(self.module_path.join("::"));
        }
        visit::visit_item_mod(self, item);
        self.module_path.pop();
    }

    fn visit_item_fn(&mut self, item: &'ast ItemFn) {
        if item.attrs.iter().any(attribute_is_executable_test) {
            self.record(&item.sig.ident.to_string(), &item.attrs);
        }
        visit::visit_item_fn(self, item);
    }

    fn visit_impl_item_fn(&mut self, item: &'ast ImplItemFn) {
        if item.attrs.iter().any(attribute_is_executable_test) {
            self.record(&format!("impl::{}", item.sig.ident), &item.attrs);
        }
        visit::visit_impl_item_fn(self, item);
    }
}

#[derive(Debug, Default)]
struct SourceInclusionVisitor {
    findings: BTreeSet<String>,
}

impl<'ast> Visit<'ast> for SourceInclusionVisitor {
    fn visit_attribute(&mut self, attribute: &'ast Attribute) {
        collect_source_path_meta(&attribute.meta, &mut self.findings);
        visit::visit_attribute(self, attribute);
    }

    fn visit_macro(&mut self, item: &'ast Macro) {
        if item
            .path
            .segments
            .last()
            .is_some_and(|segment| segment.ident == "include")
        {
            self.findings.insert(format!("include!:{}", item.tokens));
        }
        visit::visit_macro(self, item);
    }
}

#[derive(Debug, Default)]
struct PermanentlyDisabledTestVisitor {
    scan_all_attributes: bool,
    findings: BTreeSet<String>,
}

impl<'ast> Visit<'ast> for PermanentlyDisabledTestVisitor {
    fn visit_attribute(&mut self, attribute: &'ast Attribute) {
        if self.scan_all_attributes && attribute_permanently_disables_item(attribute) {
            self.findings
                .insert(cfg_identity_suffix(std::slice::from_ref(attribute)));
        }
        visit::visit_attribute(self, attribute);
    }

    fn visit_item_mod(&mut self, item: &'ast ItemMod) {
        if !self.scan_all_attributes
            && item.ident == "tests"
            && item.attrs.iter().any(attribute_permanently_disables_item)
        {
            self.findings
                .insert(format!("mod tests{}", cfg_identity_suffix(&item.attrs)));
        }
        visit::visit_item_mod(self, item);
    }
}

pub(super) fn audit_workspace(repository_root: &Path) -> io::Result<TestLayoutObservation> {
    let mut observation = TestLayoutObservation::default();
    for package_root in workspace_package_roots(repository_root)? {
        audit_package(repository_root, &package_root, &mut observation)?;
    }
    Ok(observation)
}

fn workspace_package_roots(repository_root: &Path) -> io::Result<Vec<PathBuf>> {
    let cargo = env::var_os("CARGO").unwrap_or_else(|| OsString::from("cargo"));
    let output = Command::new(cargo)
        .current_dir(repository_root)
        .args(["metadata", "--no-deps", "--format-version", "1"])
        .output()?;
    if !output.status.success() {
        return Err(io::Error::other(format!(
            "cargo metadata failed: {}",
            String::from_utf8_lossy(&output.stderr).trim()
        )));
    }
    let metadata: CargoMetadata =
        serde_json::from_slice(&output.stdout).map_err(|error| invalid_data(error.to_string()))?;
    let workspace_members = metadata
        .workspace_members
        .into_iter()
        .collect::<BTreeSet<_>>();
    let mut roots = metadata
        .packages
        .into_iter()
        .filter(|package| workspace_members.contains(&package.id))
        .map(|package| {
            package
                .manifest_path
                .parent()
                .expect("Cargo manifest has a parent directory")
                .to_path_buf()
        })
        .collect::<Vec<_>>();
    roots.sort();
    roots.dedup();
    Ok(roots)
}

fn audit_package(
    repository_root: &Path,
    package_root: &Path,
    observation: &mut TestLayoutObservation,
) -> io::Result<()> {
    let source_root = package_root.join("src");
    for path in rust_files_below(&source_root)? {
        let relative_source = path
            .strip_prefix(&source_root)
            .map_err(|error| invalid_data(error.to_string()))?;
        let repository_path = repository_relative(repository_root, &path)?;
        if has_legacy_test_suffix(&path) {
            observation.sibling_test_sources.insert(repository_path);
            continue;
        }
        let is_test_source = is_canonical_test_source(relative_source);
        let permanently_disabled = permanently_disabled_test_findings(&path, is_test_source)?;
        if !permanently_disabled.is_empty() {
            observation
                .permanently_disabled_test_sources
                .insert(repository_path.clone(), permanently_disabled);
        }
        if is_test_source {
            if is_test_tree_facade(relative_source) {
                let findings = test_facade_non_registration_items(&path)?;
                if !findings.is_empty() {
                    observation
                        .test_facade_non_registration_items
                        .insert(repository_path, findings);
                }
            }
            continue;
        }
        let inline = inline_test_findings(&path)?;
        if !inline.modules.is_empty() {
            observation
                .inline_test_modules
                .insert(repository_path.clone(), inline.modules);
        }
        if !inline.identities.is_empty() {
            observation
                .inline_executable_tests
                .insert(repository_path, inline.identities);
        }
    }

    let integration_root = package_root.join("tests");
    for path in rust_files_below(&integration_root)? {
        let repository_path = repository_relative(repository_root, &path)?;
        let permanently_disabled = permanently_disabled_test_findings(&path, true)?;
        if !permanently_disabled.is_empty() {
            observation
                .permanently_disabled_test_sources
                .insert(repository_path.clone(), permanently_disabled);
        }
        let findings = source_inclusion_findings(&path)?;
        if !findings.is_empty() {
            observation
                .crate_test_source_inclusions
                .insert(repository_path, findings);
        }
    }
    Ok(())
}

pub(super) fn rust_files_below(root: &Path) -> io::Result<Vec<PathBuf>> {
    if !root.exists() {
        return Ok(Vec::new());
    }
    let mut files = Vec::new();
    collect_rust_files(root, &mut files)?;
    files.sort();
    Ok(files)
}

fn collect_rust_files(directory: &Path, files: &mut Vec<PathBuf>) -> io::Result<()> {
    let mut entries = fs::read_dir(directory)?.collect::<Result<Vec<_>, _>>()?;
    entries.sort_by_key(std::fs::DirEntry::file_name);
    for entry in entries {
        let file_type = entry.file_type()?;
        if file_type.is_symlink() {
            return Err(invalid_data(format!(
                "test-layout refuses to skip symlinked source path: {}",
                entry.path().display()
            )));
        }
        let path = entry.path();
        if file_type.is_dir() {
            collect_rust_files(&path, files)?;
        } else if file_type.is_file() && path.extension().is_some_and(|extension| extension == "rs")
        {
            files.push(path);
        }
    }
    Ok(())
}

pub(super) fn is_canonical_test_source(relative_source: &Path) -> bool {
    relative_source
        .components()
        .any(|component| component == Component::Normal("tests".as_ref()))
        || relative_source
            .file_name()
            .is_some_and(|name| name == "tests.rs")
}

pub(super) fn is_test_tree_facade(relative_source: &Path) -> bool {
    relative_source
        .file_name()
        .is_some_and(|name| name == "mod.rs")
        && relative_source
            .parent()
            .and_then(Path::file_name)
            .is_some_and(|name| name == "tests")
}

pub(super) fn has_legacy_test_suffix(path: &Path) -> bool {
    path.file_name()
        .and_then(|name| name.to_str())
        .is_some_and(|name| name.ends_with("_test.rs") || name.ends_with("_tests.rs"))
}

fn inline_test_findings(path: &Path) -> io::Result<InlineTestVisitor> {
    let syntax = parse_rust_file(path)?;
    Ok(inline_test_findings_in_syntax(&syntax))
}

fn test_facade_non_registration_items(path: &Path) -> io::Result<BTreeSet<String>> {
    let syntax = parse_rust_file(path)?;
    Ok(test_facade_non_registration_items_in_syntax(&syntax))
}

pub(super) fn test_facade_non_registration_items_in_syntax(syntax: &File) -> BTreeSet<String> {
    syntax
        .items
        .iter()
        .filter_map(|item| match item {
            Item::Mod(module) if module.content.is_none() => None,
            Item::Mod(module) => Some(format!("inline mod {}", module.ident)),
            Item::Use(_) => Some("use declaration".to_owned()),
            Item::Fn(function) => Some(format!("fn {}", function.sig.ident)),
            Item::Const(item) => Some(format!("const {}", item.ident)),
            Item::Enum(item) => Some(format!("enum {}", item.ident)),
            Item::Static(item) => Some(format!("static {}", item.ident)),
            Item::Struct(item) => Some(format!("struct {}", item.ident)),
            Item::Trait(item) => Some(format!("trait {}", item.ident)),
            Item::Type(item) => Some(format!("type {}", item.ident)),
            _ => Some("non-module item".to_owned()),
        })
        .collect()
}

#[cfg(test)]
pub(super) fn inline_test_identities_in_syntax(syntax: &File) -> BTreeSet<String> {
    inline_test_findings_in_syntax(syntax).identities
}

#[cfg(test)]
pub(super) fn inline_test_modules_in_syntax(syntax: &File) -> BTreeSet<String> {
    inline_test_findings_in_syntax(syntax).modules
}

fn inline_test_findings_in_syntax(syntax: &File) -> InlineTestVisitor {
    let mut visitor = InlineTestVisitor::default();
    visitor.visit_file(syntax);
    visitor
}

fn source_inclusion_findings(path: &Path) -> io::Result<BTreeSet<String>> {
    let syntax = parse_rust_file(path)?;
    Ok(source_inclusion_findings_in_syntax(&syntax))
}

pub(super) fn source_inclusion_findings_in_syntax(syntax: &File) -> BTreeSet<String> {
    let mut visitor = SourceInclusionVisitor::default();
    visitor.visit_file(syntax);
    visitor.findings
}

fn permanently_disabled_test_findings(
    path: &Path,
    scan_all_attributes: bool,
) -> io::Result<BTreeSet<String>> {
    let syntax = parse_rust_file(path)?;
    Ok(permanently_disabled_test_findings_in_syntax(
        &syntax,
        scan_all_attributes,
    ))
}

pub(super) fn permanently_disabled_test_findings_in_syntax(
    syntax: &File,
    scan_all_attributes: bool,
) -> BTreeSet<String> {
    let mut visitor = PermanentlyDisabledTestVisitor {
        scan_all_attributes,
        ..PermanentlyDisabledTestVisitor::default()
    };
    visitor.visit_file(syntax);
    visitor.findings
}

fn parse_rust_file(path: &Path) -> io::Result<File> {
    let source = fs::read_to_string(path)?;
    syn::parse_file(&source)
        .map_err(|error| invalid_data(format!("cannot parse {}: {error}", path.display())))
}

fn attribute_is_executable_test(attribute: &Attribute) -> bool {
    meta_is_executable_test(&attribute.meta)
}

fn meta_is_executable_test(meta: &Meta) -> bool {
    if meta.path().segments.last().is_some_and(|segment| {
        matches!(
            segment.ident.to_string().as_str(),
            "test" | "rstest" | "test_case"
        )
    }) {
        return true;
    }
    if !meta.path().is_ident("cfg_attr") {
        return false;
    }
    nested_meta(meta).is_some_and(|nested| nested.iter().skip(1).any(meta_is_executable_test))
}

fn attribute_is_test_cfg(attribute: &Attribute) -> bool {
    if attribute.path().is_ident("cfg") {
        return nested_meta(&attribute.meta)
            .is_some_and(|nested| nested.iter().any(cfg_expression_enables_test));
    }
    attribute.path().is_ident("cfg_attr")
        && nested_meta(&attribute.meta).is_some_and(|nested| {
            nested.iter().skip(1).any(|meta| {
                meta.path().is_ident("cfg")
                    && nested_meta(meta)
                        .is_some_and(|cfg| cfg.iter().any(cfg_expression_enables_test))
            })
        })
}

fn cfg_expression_enables_test(meta: &Meta) -> bool {
    if meta.path().is_ident("test") {
        return true;
    }
    if meta.path().is_ident("not") {
        return false;
    }
    (meta.path().is_ident("all") || meta.path().is_ident("any"))
        && nested_meta(meta).is_some_and(|nested| nested.iter().any(cfg_expression_enables_test))
}

fn attribute_permanently_disables_item(attribute: &Attribute) -> bool {
    if attribute.path().is_ident("cfg") {
        return nested_meta(&attribute.meta).is_some_and(|nested| {
            nested.len() == 1
                && nested
                    .first()
                    .and_then(cfg_expression_constant)
                    .is_some_and(|value| !value)
        });
    }
    if !attribute.path().is_ident("cfg_attr") {
        return false;
    }
    nested_meta(&attribute.meta).is_some_and(|nested| {
        let mut nested = nested.iter();
        nested
            .next()
            .and_then(cfg_expression_constant)
            .is_some_and(|value| value)
            && nested.any(meta_permanently_disables_item)
    })
}

fn meta_permanently_disables_item(meta: &Meta) -> bool {
    if !meta.path().is_ident("cfg") {
        return false;
    }
    nested_meta(meta).is_some_and(|nested| {
        nested.len() == 1
            && nested
                .first()
                .and_then(cfg_expression_constant)
                .is_some_and(|value| !value)
    })
}

fn cfg_expression_constant(meta: &Meta) -> Option<bool> {
    if meta.path().is_ident("not") {
        let nested = nested_meta(meta)?;
        return (nested.len() == 1)
            .then(|| {
                nested
                    .first()
                    .and_then(cfg_expression_constant)
                    .map(|value| !value)
            })
            .flatten();
    }
    if meta.path().is_ident("all") {
        let nested = nested_meta(meta)?;
        let mut has_unknown = false;
        for item in &nested {
            match cfg_expression_constant(item) {
                Some(false) => return Some(false),
                Some(true) => {}
                None => has_unknown = true,
            }
        }
        return (!has_unknown).then_some(true);
    }
    if meta.path().is_ident("any") {
        let nested = nested_meta(meta)?;
        let mut has_unknown = false;
        for item in &nested {
            match cfg_expression_constant(item) {
                Some(true) => return Some(true),
                Some(false) => {}
                None => has_unknown = true,
            }
        }
        return (!has_unknown).then_some(false);
    }
    None
}

fn nested_meta(meta: &Meta) -> Option<Punctuated<Meta, Token![,]>> {
    let Meta::List(list) = meta else {
        return None;
    };
    list.parse_args_with(Punctuated::<Meta, Token![,]>::parse_terminated)
        .ok()
}

fn collect_source_path_meta(meta: &Meta, findings: &mut BTreeSet<String>) {
    if meta.path().is_ident("path") {
        let target = match meta {
            Meta::NameValue(name_value) => match &name_value.value {
                Expr::Lit(expression) => match &expression.lit {
                    Lit::Str(value) => value.value(),
                    _ => "<non-string>".to_owned(),
                },
                _ => "<non-literal>".to_owned(),
            },
            _ => "<malformed>".to_owned(),
        };
        findings.insert(format!("path:{target}"));
        return;
    }
    if meta.path().is_ident("cfg_attr")
        && let Some(nested) = nested_meta(meta)
    {
        for attribute in nested.iter().skip(1) {
            collect_source_path_meta(attribute, findings);
        }
    }
}

fn cfg_identity_suffix(attributes: &[Attribute]) -> String {
    attributes
        .iter()
        .filter_map(|attribute| match &attribute.meta {
            Meta::List(list)
                if attribute.path().is_ident("cfg") || attribute.path().is_ident("cfg_attr") =>
            {
                Some(format!(
                    "#[{}({})]",
                    path_identity(attribute.path()),
                    list.tokens
                ))
            }
            _ => None,
        })
        .collect()
}

fn path_identity(path: &syn::Path) -> String {
    path.segments
        .iter()
        .map(|segment| segment.ident.to_string())
        .collect::<Vec<_>>()
        .join("::")
}

pub(super) fn finding_count(findings: &BTreeMap<String, BTreeSet<String>>) -> usize {
    findings.values().map(BTreeSet::len).sum()
}

fn repository_relative(repository_root: &Path, path: &Path) -> io::Result<String> {
    let relative = path
        .strip_prefix(repository_root)
        .map_err(|error| invalid_data(error.to_string()))?;
    Ok(relative
        .components()
        .map(|component| component.as_os_str().to_string_lossy())
        .collect::<Vec<_>>()
        .join("/"))
}
