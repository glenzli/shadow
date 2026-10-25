use std::{
    fs,
    path::{Path, PathBuf},
    sync::atomic::Ordering,
};

use rusqlite::Connection;
use shadow_catalog::RegisterAsset;
use shadow_domain::RepresentationKind;
use shadow_native_path::native_location;

use super::{ProfileArchiveControl, manifest, run};
use crate::{DesktopSession, open_desktop_session_at};

struct Fixture {
    temp: tempfile::TempDir,
    root: PathBuf,
    original: PathBuf,
    photo: String,
    session: Box<DesktopSession>,
}

impl Fixture {
    fn new() -> Self {
        let temp = tempfile::tempdir().unwrap();
        let root = temp.path().join("source-library");
        let original = temp.path().join("original.dng");
        fs::create_dir(&root).unwrap();
        fs::write(root.join("storage-profile.json"), serde_json::to_vec(&serde_json::json!({
            "schemaVersion":1,"id":uuid::Uuid::now_v7().to_string(),"kind":"custom","displayName":"Source","desktopSettingsMigrated":true
        })).unwrap()).unwrap();
        fs::write(&original, [0u8; 1024]).unwrap();
        let session =
            open_desktop_session_at(&root.join("catalog.sqlite"), &root.join("cache")).unwrap();
        let registered = session
            .catalog
            .register_asset(&RegisterAsset {
                kind: RepresentationKind::OriginalRaw,
                location: native_location(&original),
                byte_len: 1024,
                modified_at_ms: None,
                now_ms: 1,
            })
            .unwrap();
        let photo = registered.photo_id.to_string();
        let mut draft = crate::recipe_v1::GradeStackDraft::default();
        draft.grade_nodes[0].basic.exposure_stops = 0.25;
        let settings = crate::recipe_v1::encode_grade_stack_draft_recipe_v1(draft).unwrap();
        session
            .save_basic_edit_version_at(
                &photo,
                original.to_str().unwrap(),
                "",
                &settings,
                "Preserved look",
                1000,
            )
            .unwrap();
        fs::write(root.join("ui-preferences.ini"), "[ai]\nimage_understanding_execution_allowed=true\nimage_understanding_background_enabled=true\n[ui]\nlanguage=zh_CN\n[library-server]\nauto_start=true\n").unwrap();
        fs::create_dir_all(root.join("derived-rasters")).unwrap();
        fs::write(
            root.join("derived-rasters/attachment.bin"),
            b"author-owned attachment",
        )
        .unwrap();
        Self {
            temp,
            root,
            original,
            photo,
            session,
        }
    }
    fn archive(&self) -> PathBuf {
        run(
            0,
            &self.root,
            self.temp.path(),
            "",
            "",
            &ProfileArchiveControl::default(),
        )
        .unwrap()
        .0
    }
}

#[test]
fn verified_snapshot_restores_real_edit_heads_attachments_and_wal_sidecars_independently() {
    let fixture = Fixture::new();
    let history = fixture
        .session
        .photo_edit_state(&fixture.photo, fixture.original.to_str().unwrap())
        .unwrap();
    let sidecar = fixture.root.join("notes.sqlite");
    let connection = Connection::open(&sidecar).unwrap();
    connection.execute_batch("PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0; CREATE TABLE notes(value TEXT); INSERT INTO notes VALUES('latest WAL value');").unwrap();
    let archive = fixture.archive();
    let control = ProfileArchiveControl::default();
    let attachment_time = fs::metadata(fixture.root.join("derived-rasters/attachment.bin"))
        .unwrap()
        .modified()
        .unwrap();
    fs::File::options()
        .write(true)
        .open(archive.join("payload/derived-rasters/attachment.bin"))
        .unwrap()
        .set_times(fs::FileTimes::new().set_modified(std::time::UNIX_EPOCH))
        .unwrap();
    let verified = manifest::verify(&archive, &control).unwrap();
    assert!(
        verified
            .files
            .iter()
            .any(
                |f| manifest::relative_path(&f.path).unwrap() == Path::new("notes.sqlite")
                    && f.sqlite
            )
    );
    assert!(!archive.join("payload/notes.sqlite-wal").exists());
    let restored = run(1, &archive, fixture.temp.path(), "Restored", "", &control)
        .unwrap()
        .0;
    let restored_session =
        open_desktop_session_at(&restored.join("catalog.sqlite"), &restored.join("cache")).unwrap();
    let restored_history = restored_session
        .photo_edit_state(&fixture.photo, fixture.original.to_str().unwrap())
        .unwrap();
    assert_eq!(
        restored_history.working_commit_id,
        history.working_commit_id
    );
    assert_eq!(
        restored_history.settings.grade_nodes[0]
            .basic
            .exposure_stops,
        0.25
    );
    assert_eq!(
        fs::read(restored.join("derived-rasters/attachment.bin")).unwrap(),
        b"author-owned attachment"
    );
    assert_eq!(
        fs::metadata(restored.join("derived-rasters/attachment.bin"))
            .unwrap()
            .modified()
            .unwrap(),
        attachment_time
    );
    assert_ne!(
        manifest::profile(&restored, "").unwrap()["id"],
        manifest::profile(&fixture.root, "").unwrap()["id"]
    );
    assert_eq!(
        Connection::open(restored.join("notes.sqlite"))
            .unwrap()
            .query_row("SELECT value FROM notes", [], |r| r.get::<_, String>(0))
            .unwrap(),
        "latest WAL value"
    );
    fs::write(
        restored.join("derived-rasters/attachment.bin"),
        b"copy edit",
    )
    .unwrap();
    assert_eq!(
        fs::read(fixture.root.join("derived-rasters/attachment.bin")).unwrap(),
        b"author-owned attachment"
    );
    assert_eq!(fs::read(&fixture.original).unwrap(), vec![0u8; 1024]);
    manifest::verify(&archive, &control).unwrap();
    drop(restored_session);
    drop(connection);
    drop(fixture.session);
    fs::remove_dir_all(&fixture.root).unwrap();
    let recovered = run(
        1,
        &archive,
        fixture.temp.path(),
        "Recovered without source",
        "",
        &control,
    )
    .unwrap()
    .0;
    let recovered_session =
        open_desktop_session_at(&recovered.join("catalog.sqlite"), &recovered.join("cache"))
            .unwrap();
    assert_eq!(
        recovered_session
            .photo_edit_state(&fixture.photo, fixture.original.to_str().unwrap())
            .unwrap()
            .working_commit_id,
        history.working_commit_id
    );
}

#[test]
fn testing_copy_rebases_owned_locations_and_retires_pending_work_without_changing_history() {
    let fixture = Fixture::new();
    let owned = fixture.root.join("remote-originals/local.dng");
    fs::create_dir_all(owned.parent().unwrap()).unwrap();
    fs::write(&owned, b"materialized-original").unwrap();
    let fingerprint = shadow_core::fingerprint_source(&owned).unwrap();
    let registered = fixture
        .session
        .catalog
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: native_location(&owned),
            byte_len: 21,
            modified_at_ms: fingerprint.modified_at_ms,
            now_ms: 2,
        })
        .unwrap();
    let connection = Connection::open(fixture.root.join("catalog.sqlite")).unwrap();
    fixture.session.smart_classification_snapshot().unwrap();
    let categories = Connection::open(
        fixture
            .root
            .join("cache/semantic-index/smart-categories-v1.sqlite3"),
    )
    .unwrap();
    categories
        .execute_batch("UPDATE adaptation_state SET requested_revision=2,applied_revision=1;")
        .unwrap();
    let job = uuid::Uuid::now_v7();
    connection.execute("INSERT INTO export_jobs(id,state,settings_json,settings_digest,created_at_ms,updated_at_ms) VALUES(?1,'queued','{}',zeroblob(32),1,1)",[job.as_bytes().as_slice()]).unwrap();
    let copy = run(
        2,
        &fixture.root,
        fixture.temp.path(),
        "Test",
        "",
        &ProfileArchiveControl::default(),
    )
    .unwrap()
    .0;
    let restored =
        open_desktop_session_at(&copy.join("catalog.sqlite"), &copy.join("cache")).unwrap();
    assert_eq!(
        shadow_core::fingerprint_source(&copy.join("remote-originals/local.dng")).unwrap(),
        fingerprint
    );
    let location = restored
        .catalog
        .review_source(registered.photo_id)
        .unwrap()
        .unwrap()
        .location;
    assert_eq!(
        shadow_native_path::native_path_from_location(&location).unwrap(),
        copy.join("remote-originals/local.dng")
    );
    let state: String = Connection::open(copy.join("catalog.sqlite"))
        .unwrap()
        .query_row(
            "SELECT state FROM export_jobs WHERE id=?1",
            [job.as_bytes().as_slice()],
            |r| r.get(0),
        )
        .unwrap();
    assert_eq!(state, "cancelled");
    assert_eq!(
        connection
            .query_row(
                "SELECT state FROM export_jobs WHERE id=?1",
                [job.as_bytes().as_slice()],
                |r| r.get::<_, String>(0)
            )
            .unwrap(),
        "queued"
    );
    let preferences = fs::read_to_string(copy.join("ui-preferences.ini")).unwrap();
    assert!(preferences.contains("image_understanding_background_enabled=false"));
    assert!(preferences.contains("auto_start=false"));
    assert!(preferences.contains("language=zh_CN"));
    assert!(
        !restored
            .smart_classification_snapshot()
            .unwrap()
            .adaptation_pending
    );
    assert!(
        fixture
            .session
            .smart_classification_snapshot()
            .unwrap()
            .adaptation_pending
    );
    assert_eq!(
        restored
            .photo_edit_state(&fixture.photo, fixture.original.to_str().unwrap())
            .unwrap()
            .working_commit_id,
        fixture
            .session
            .photo_edit_state(&fixture.photo, fixture.original.to_str().unwrap())
            .unwrap()
            .working_commit_id
    );
}

#[test]
fn corrupt_missing_extra_and_traversing_backup_entries_fail_before_new_library_publication() {
    let fixture = Fixture::new();
    let control = ProfileArchiveControl::default();
    let archive = fixture.archive();
    let bytes = fs::read(archive.join("payload/derived-rasters/attachment.bin")).unwrap();
    fs::write(
        archive.join("payload/derived-rasters/attachment.bin"),
        b"changed",
    )
    .unwrap();
    let before = fs::read_dir(fixture.temp.path()).unwrap().count();
    assert!(run(1, &archive, fixture.temp.path(), "Restored", "", &control).is_err());
    assert_eq!(fs::read_dir(fixture.temp.path()).unwrap().count(), before);
    fs::write(
        archive.join("payload/derived-rasters/attachment.bin"),
        bytes,
    )
    .unwrap();
    fs::write(
        archive.join("payload/catalog.sqlite-wal"),
        b"unlisted SQLite sidecar",
    )
    .unwrap();
    assert!(manifest::verify(&archive, &control).is_err());
    fs::remove_file(archive.join("payload/catalog.sqlite-wal")).unwrap();
    fs::remove_file(archive.join("payload/derived-rasters/attachment.bin")).unwrap();
    assert!(manifest::verify(&archive, &control).is_err());
    let unsafe_entry = native_location(Path::new("../outside"));
    assert!(manifest::relative_path(&unsafe_entry).is_err());
}

#[test]
fn cancelled_wrong_identity_and_nested_destinations_preserve_the_source() {
    let fixture = Fixture::new();
    let control = ProfileArchiveControl::default();
    control.cancelled.store(true, Ordering::Release);
    assert!(run(0, &fixture.root, fixture.temp.path(), "", "", &control).is_err());
    let control = ProfileArchiveControl::default();
    assert!(
        run(
            0,
            &fixture.root,
            fixture.temp.path(),
            "",
            &uuid::Uuid::now_v7().to_string(),
            &control
        )
        .is_err()
    );
    assert!(run(0, &fixture.root, &fixture.root, "", "", &control).is_err());
    assert_eq!(
        fs::read(fixture.root.join("derived-rasters/attachment.bin")).unwrap(),
        b"author-owned attachment"
    );
}

#[cfg(unix)]
#[test]
fn symlinks_are_rejected_and_native_attachment_names_round_trip_losslessly() {
    use std::{
        ffi::OsString,
        os::unix::{ffi::OsStringExt, fs::symlink},
    };
    let fixture = Fixture::new();
    symlink(&fixture.original, fixture.root.join("linked-original")).unwrap();
    assert!(
        run(
            0,
            &fixture.root,
            fixture.temp.path(),
            "",
            "",
            &ProfileArchiveControl::default()
        )
        .is_err()
    );
    fs::remove_file(fixture.root.join("linked-original")).unwrap();
    let native = OsString::from_vec(b"attachment-\xff".to_vec());
    assert_eq!(
        manifest::relative_path(&native_location(Path::new(&native))).unwrap(),
        Path::new(&native)
    );
    // APFS rejects invalid UTF-8 filenames; lossless encoding is checked above,
    // while filesystem I/O uses a supported decomposed Unicode filename.
    let name = OsString::from("附件-e\u{301}.bin");
    fs::write(fixture.root.join(&name), b"native bytes").unwrap();
    let archive = fixture.archive();
    let copy = run(
        1,
        &archive,
        fixture.temp.path(),
        "Restored",
        "",
        &ProfileArchiveControl::default(),
    )
    .unwrap()
    .0;
    assert_eq!(fs::read(copy.join(name)).unwrap(), b"native bytes");
}

#[test]
fn failed_restore_after_staging_removes_only_its_new_directory() {
    let fixture = Fixture::new();
    fs::write(fixture.root.join("ui-preferences.ini"), [0xff]).unwrap();
    let archive = fixture.archive();
    let before = fs::read_dir(fixture.temp.path()).unwrap().count();
    assert!(
        run(
            1,
            &archive,
            fixture.temp.path(),
            "Restored",
            "",
            &ProfileArchiveControl::default()
        )
        .is_err()
    );
    assert_eq!(fs::read_dir(fixture.temp.path()).unwrap().count(), before);
    assert_eq!(
        fs::read(fixture.root.join("ui-preferences.ini")).unwrap(),
        vec![0xff]
    );
    manifest::verify(&archive, &ProfileArchiveControl::default()).unwrap();
}
