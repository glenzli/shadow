use std::{fs, path::PathBuf};

use shadow_catalog::RegisterAsset;
use shadow_domain::RepresentationKind;
use shadow_native_path::native_location;

use super::archive;
use crate::{
    DesktopSession, ffi, open_desktop_session_at,
    recipe_v1::{GradeStackDraft, encode_grade_stack_draft_recipe_v1},
};

struct Fixture {
    _temp: tempfile::TempDir,
    root: PathBuf,
    original: PathBuf,
    photo: String,
    session: Box<DesktopSession>,
}

impl Fixture {
    fn new() -> Self {
        let temp = tempfile::tempdir().unwrap();
        let root = temp.path().join("private-session");
        fs::create_dir(&root).unwrap();
        fs::write(
            root.join(".shadow-document-session"),
            uuid::Uuid::now_v7().to_string(),
        )
        .unwrap();
        let original = temp.path().join("original.dng");
        fs::write(&original, b"immutable original source").unwrap();
        let session =
            open_desktop_session_at(&root.join("catalog.sqlite"), &root.join("cache")).unwrap();
        let registered = session
            .catalog
            .register_asset(&RegisterAsset {
                kind: RepresentationKind::OriginalRaw,
                location: native_location(&original),
                byte_len: 25,
                modified_at_ms: None,
                now_ms: 1,
            })
            .unwrap();
        let photo = registered.photo_id.to_string();
        let settings = encode_grade_stack_draft_recipe_v1(GradeStackDraft::default()).unwrap();
        session
            .save_basic_edit_version_at(
                &photo,
                original.to_str().unwrap(),
                "",
                &settings,
                "Initial",
                1,
            )
            .unwrap();
        fs::create_dir_all(root.join("renders")).unwrap();
        fs::write(
            root.join("renders").join(format!("{photo}.png")),
            b"render bytes",
        )
        .unwrap();
        fs::create_dir_all(root.join("derived-rasters/objects/v1/b3/ab")).unwrap();
        fs::write(
            root.join("derived-rasters/objects/v1/b3/ab/resource"),
            b"accepted mask or AI pixels",
        )
        .unwrap();
        fs::write(root.join("credentials.token"), b"must never be packaged").unwrap();
        Self {
            _temp: temp,
            root,
            original,
            photo,
            session,
        }
    }
    fn path(&self, name: &str) -> PathBuf {
        self._temp.path().join(name)
    }
}

#[test]
fn document_reopens_after_original_and_session_are_removed() {
    let f = Fixture::new();
    let package = f.path("photo.shdw");
    let digest = archive::pack(&f.root, &package, "").unwrap();
    assert_eq!(digest.len(), 64);
    fs::remove_file(&f.original).unwrap();
    let target = tempfile::tempdir().unwrap();
    let restored = archive::unpack(&package, target.path()).unwrap();
    assert_eq!(restored.len(), 1);
    assert_eq!(restored[0].photo_id, f.photo);
    assert_eq!(
        fs::read(&restored[0].source_path).unwrap(),
        b"immutable original source"
    );
    assert!(!target.path().join("credentials.token").exists());
    assert_eq!(
        fs::read(
            target
                .path()
                .join("derived-rasters/objects/v1/b3/ab/resource")
        )
        .unwrap(),
        b"accepted mask or AI pixels"
    );
    let reopened = open_desktop_session_at(
        &target.path().join("catalog.sqlite"),
        &target.path().join("cache"),
    )
    .unwrap();
    let state = reopened
        .photo_edit_state(&f.photo, &restored[0].source_path)
        .unwrap();
    assert_eq!(state.versions.len(), 1);
    assert!(state.has_working_version);
}

#[test]
fn publication_rejects_existing_and_changed_documents_without_damage() {
    let f = Fixture::new();
    let package = f.path("photo.shdw");
    let digest = archive::pack(&f.root, &package, "").unwrap();
    let before = fs::read(&package).unwrap();
    assert!(archive::pack(&f.root, &package, "").is_err());
    assert_eq!(fs::read(&package).unwrap(), before);
    fs::write(&package, b"another owner's revision").unwrap();
    assert!(archive::pack(&f.root, &package, &digest).is_err());
    assert_eq!(fs::read(&package).unwrap(), b"another owner's revision");
}

#[test]
fn document_history_retains_evicted_actions_and_undo_branches() {
    let f = Fixture::new();
    let mut before = GradeStackDraft::default();
    for index in 0..260 {
        let mut after = before.clone();
        after.grade_nodes[0].basic.exposure_stops = (index + 1) as f64 / 100.0;
        let entry = ffi::FfiDocumentHistoryEntry {
            key: "exposure".into(),
            before_base: String::new(),
            before: encode_grade_stack_draft_recipe_v1(before.clone()).unwrap(),
            after_base: String::new(),
            after: encode_grade_stack_draft_recipe_v1(after.clone()).unwrap(),
        };
        f.session
            .record_document_history(&f.photo, f.original.to_str().unwrap(), 0, &entry)
            .unwrap();
        before = after;
    }
    let undo = f.session.document_undo(&f.photo).unwrap();
    assert_eq!(undo.undo.len(), 256);
    let last = undo.undo.last().unwrap();
    let reverted = ffi::FfiDocumentHistoryEntry {
        key: last.key.clone(),
        before_base: last.after_base.clone(),
        before: encode_grade_stack_draft_recipe_v1(before.clone()).unwrap(),
        after_base: last.before_base.clone(),
        after: encode_grade_stack_draft_recipe_v1({
            let mut v = before.clone();
            v.grade_nodes[0].basic.exposure_stops = 2.59;
            v
        })
        .unwrap(),
    };
    f.session
        .record_document_history(&f.photo, f.original.to_str().unwrap(), 1, &reverted)
        .unwrap();
    assert_eq!(f.session.document_undo(&f.photo).unwrap().redo.len(), 1);
    let page = history_items(&f.session, &f.photo);
    let oldest = page.last().unwrap()["id"]
        .as_str()
        .unwrap()
        .parse()
        .unwrap();
    let step = f
        .session
        .checkout_document_history(&f.photo, oldest)
        .unwrap();
    assert_eq!(step.key, oldest.to_string());
    let package = f.path("history.shdw");
    archive::pack(&f.root, &package, "").unwrap();
    let target = tempfile::tempdir().unwrap();
    let inputs = archive::unpack(&package, target.path()).unwrap();
    let reopened = open_desktop_session_at(
        &target.path().join("catalog.sqlite"),
        &target.path().join("cache"),
    )
    .unwrap();
    assert_eq!(
        reopened
            .document_undo(&inputs[0].photo_id)
            .unwrap()
            .redo
            .len(),
        1
    );
    let saved = history_items(&reopened, &f.photo);
    assert_eq!(saved.len(), 260);
}

#[cfg(unix)]
#[test]
fn document_resources_cannot_follow_symlinks() {
    let f = Fixture::new();
    std::os::unix::fs::symlink(&f.original, f.root.join("derived-rasters/objects/linked")).unwrap();
    assert!(archive::pack(&f.root, &f.path("unsafe.shdw"), "").is_err());
    assert!(!f.path("unsafe.shdw").exists());
}

fn history_items(session: &DesktopSession, photo: &str) -> Vec<serde_json::Value> {
    let mut result = Vec::new();
    let mut cursor = 0;
    loop {
        let page: serde_json::Value =
            serde_json::from_str(&session.document_history_page(photo, cursor).unwrap()).unwrap();
        result.extend(page["items"].as_array().unwrap().clone());
        let Some(next) = page["nextCursor"].as_str() else {
            break;
        };
        cursor = next.parse().unwrap();
    }
    result
}

#[test]
fn managed_mask_and_lut_relocate_without_losing_editability() {
    use sha2::{Digest, Sha256};
    use shadow_domain::{ManagedRasterMask, MaskDefinition, RasterMaskEncoding};
    let f = Fixture::new();
    let pixels = vec![0_u8, 64, 128, 255];
    let hash = blake3::hash(&pixels).to_hex().to_string();
    let object = format!("objects/v1/b3/{}/{}", &hash[..2], &hash[2..]);
    let resource = f.root.join("derived-rasters").join(&object);
    fs::create_dir_all(resource.parent().unwrap()).unwrap();
    fs::write(&resource, &pixels).unwrap();
    let mask = ManagedRasterMask::new(
        object,
        1,
        hash,
        4,
        2,
        2,
        32,
        24,
        RasterMaskEncoding::Gray8Unorm,
    )
    .unwrap();
    let lut = "LUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n";
    let lut_id = format!("{:x}", Sha256::digest(lut.as_bytes()));
    let lut_path = f.root.join("lut-store").join(format!("{lut_id}.cube"));
    fs::create_dir_all(lut_path.parent().unwrap()).unwrap();
    fs::write(&lut_path, lut).unwrap();
    let mut draft = GradeStackDraft::default();
    let before = draft.clone();
    draft.grade_nodes[0].local_mask =
        Some(MaskDefinition::managed_raster(mask.clone(), false).unwrap());
    draft.grade_nodes[0].fine.lut.resource_id = lut_id.clone();
    draft.grade_nodes[0].fine.lut.managed_path = lut_path.to_str().unwrap().into();
    draft.grade_nodes[0].fine.lut.title = "Portable LUT".into();
    let base = f
        .session
        .photo_edit_state(&f.photo, f.original.to_str().unwrap())
        .unwrap()
        .working_commit_id;
    f.session
        .autosave_grade_stack_working_at(
            &f.photo,
            f.original.to_str().unwrap(),
            &base,
            &base,
            &draft,
            2,
        )
        .unwrap();
    let state = f
        .session
        .photo_edit_state(&f.photo, f.original.to_str().unwrap())
        .unwrap();
    f.session
        .record_document_history(
            &f.photo,
            f.original.to_str().unwrap(),
            0,
            &ffi::FfiDocumentHistoryEntry {
                key: "mask/lut".into(),
                before_base: base.clone(),
                before: encode_grade_stack_draft_recipe_v1(before).unwrap(),
                after_base: state.working_commit_id,
                after: state.settings,
            },
        )
        .unwrap();
    let package = f.path("resources.shdw");
    archive::pack(&f.root, &package, "").unwrap();
    let target = tempfile::tempdir().unwrap();
    let sources = archive::unpack(&package, target.path()).unwrap();
    let reopened = open_desktop_session_at(
        &target.path().join("catalog.sqlite"),
        &target.path().join("cache"),
    )
    .unwrap();
    let state = reopened
        .photo_edit_state(&f.photo, &sources[0].source_path)
        .unwrap();
    let step = reopened
        .document_undo(&f.photo)
        .unwrap()
        .undo
        .pop()
        .unwrap();
    reopened
        .record_document_history(
            &f.photo,
            &sources[0].source_path,
            1,
            &ffi::FfiDocumentHistoryEntry {
                key: step.key,
                before_base: step.after_base,
                before: step.after,
                after_base: step.before_base,
                after: step.before,
            },
        )
        .unwrap();
    let step = reopened
        .document_undo(&f.photo)
        .unwrap()
        .redo
        .pop()
        .unwrap();
    reopened
        .record_document_history(&f.photo, &sources[0].source_path, 2, &step)
        .unwrap();
    let mut invalid = reopened.checkout_document_history(&f.photo, 1).unwrap();
    invalid.after.grade_nodes[0].label = "Different authored node".into();
    assert!(
        reopened
            .record_document_history(&f.photo, &sources[0].source_path, 3, &invalid)
            .is_err()
    );
    assert_eq!(state.settings.grade_nodes[0].fine.lut_resource_id, lut_id);
    assert_eq!(
        PathBuf::from(&state.settings.grade_nodes[0].fine.lut_managed_path),
        target
            .path()
            .join("lut-store")
            .join(format!("{lut_id}.cube"))
    );
    let store =
        shadow_core::FilesystemDerivedRasterStore::open(target.path().join("derived-rasters"))
            .unwrap();
    let mut file = store.open_recipe_mask(&mask).unwrap();
    let mut bytes = Vec::new();
    std::io::Read::read_to_end(&mut file, &mut bytes).unwrap();
    assert_eq!(bytes, pixels);
    let base = reopened
        .catalog
        .recipe_commit(
            f.photo.parse().unwrap(),
            state.working_commit_id.parse().unwrap(),
        )
        .unwrap()
        .unwrap();
    let decoded = crate::recipe_v1::decode_grade_stack_draft_recipe_v1(&state.settings).unwrap();
    let restored =
        crate::recipe_v1::grade_stack_recipe_v1_snapshot(&decoded, Some(base.commit.snapshot()))
            .unwrap();
    assert_eq!(restored.masks(), base.commit.snapshot().masks());
    // A historical resource is still required after the current node stops
    // using it. Missing bytes must prevent publication of a "complete" file.
    let current = f
        .session
        .photo_edit_state(&f.photo, f.original.to_str().unwrap())
        .unwrap();
    let mut neutral =
        crate::recipe_v1::decode_grade_stack_draft_from_recipe_v1_snapshot(base.commit.snapshot())
            .unwrap();
    neutral.grade_nodes[0].local_mask = None;
    neutral.grade_nodes[0].fine.lut = crate::recipe_v1::LutEditParameters::default();
    f.session
        .autosave_grade_stack_working_at(
            &f.photo,
            f.original.to_str().unwrap(),
            &current.working_commit_id,
            &current.working_commit_id,
            &neutral,
            3,
        )
        .unwrap();
    fs::remove_file(resource).unwrap();
    assert!(archive::pack(&f.root, &f.path("missing-old-mask.shdw"), "").is_err());
    assert!(!f.path("missing-old-mask.shdw").exists());
}
