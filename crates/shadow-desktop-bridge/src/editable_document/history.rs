//! Append-only semantic steps with movable undo/redo cursors. The native
//! adapter serializes calls off the GUI thread. Evicting an in-memory undo
//! entry never deletes a document step or its abandoned branch.

use std::io::Write;
use std::path::Path;

use anyhow::{Context, Result, ensure};
use rusqlite::{Connection, OptionalExtension, params};
use serde::{Deserialize, Serialize};
use shadow_domain::{PhotoId, RecipeCommitId, RecipeSnapshot};

use crate::{
    DesktopSession, ffi,
    recipe_v1::{
        decode_grade_stack_draft_from_recipe_v1_snapshot, decode_grade_stack_draft_recipe_v1,
        encode_grade_stack_draft_recipe_v1, grade_stack_recipe_v1_snapshot,
    },
};

#[derive(Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct State {
    base: String,
    snapshot: RecipeSnapshot,
}

impl DesktopSession {
    pub(crate) fn record_document_history(
        &self,
        photo_id: &str,
        source_path: &str,
        action: u8,
        entry: &ffi::FfiDocumentHistoryEntry,
    ) -> Result<()> {
        let Some(mut db) = open(&self.cache_root)? else {
            return Ok(());
        };
        self.validated_photo_source(photo_id, source_path)?;
        let before = self.document_state(photo_id, &entry.before_base, &entry.before)?;
        let after = self.document_state(photo_id, &entry.after_base, &entry.after)?;
        let tx = db.transaction()?;
        let (mut cursor, redo) = position(&tx, photo_id)?;
        let mut redo: Vec<i64> = serde_json::from_str(&redo)?;
        let next;
        if action == 0 || action == 4 {
            let previous = cursor
                .map(|id| {
                    step(&tx, photo_id, id)
                        .and_then(|(_, _, _, after)| Ok(serde_json::from_str::<State>(&after)?))
                })
                .transpose()?;
            if action == 4
                && (previous.is_none()
                    || equivalent_snapshot(
                        &previous.as_ref().expect("present current step").snapshot,
                        &after.snapshot,
                    )?)
            {
                return Ok(());
            }
            if action == 0 {
                if let Some(previous) = previous.as_ref() {
                    if !equivalent_snapshot(&previous.snapshot, &before.snapshot)? {
                        tx.execute("INSERT INTO steps(photo,parent,action,before_state,after_state) VALUES(?1,?2,'selection',?3,?4)",params![photo_id,cursor,serde_json::to_string(previous)?,serde_json::to_string(&before)?])?;
                        cursor = Some(tx.last_insert_rowid());
                    }
                }
            }
            let before = if action == 4 {
                previous.as_ref().expect("present current step")
            } else {
                &before
            };
            let before = serde_json::to_string(&before)?;
            let after = serde_json::to_string(&after)?;
            tx.execute("INSERT INTO steps(photo,parent,action,before_state,after_state) VALUES(?1,?2,?3,?4,?5)", params![photo_id,cursor,if action==4 {"selection"}else{&entry.key},before,after])?;
            next = Some(tx.last_insert_rowid());
            redo.clear();
        } else if action == 1 {
            let id = cursor.context("document history has no undo step")?;
            let (parent, state): (Option<i64>, String) = tx.query_row(
                "SELECT parent,before_state FROM steps WHERE id=?1 AND photo=?2",
                params![id, photo_id],
                |r| Ok((r.get(0)?, r.get(1)?)),
            )?;
            let state: State = serde_json::from_str(&state)?;
            ensure!(
                equivalent_snapshot(&state.snapshot, &after.snapshot)?,
                "document undo cursor differs from the edit"
            );
            redo.push(id);
            next = parent;
        } else if action == 2 {
            let id = redo.pop().context("document history has no redo step")?;
            let state: String = tx.query_row(
                "SELECT after_state FROM steps WHERE id=?1 AND photo=?2",
                params![id, photo_id],
                |r| r.get(0),
            )?;
            let state: State = serde_json::from_str(&state)?;
            ensure!(
                equivalent_snapshot(&state.snapshot, &after.snapshot)?,
                "document redo cursor differs from the edit"
            );
            next = Some(id);
        } else {
            ensure!(action == 3, "unknown document history movement");
            let id = entry.key.parse::<i64>()?;
            let state: String = tx.query_row(
                "SELECT after_state FROM steps WHERE id=?1 AND photo=?2",
                params![id, photo_id],
                |r| r.get(0),
            )?;
            let state: State = serde_json::from_str(&state)?;
            ensure!(
                equivalent_snapshot(&state.snapshot, &after.snapshot)?,
                "document checkout snapshot changed"
            );
            next = Some(id);
            redo.clear();
        }
        tx.execute("INSERT INTO cursors(photo,step,redo) VALUES(?1,?2,?3) ON CONFLICT(photo) DO UPDATE SET step=excluded.step,redo=excluded.redo",params![photo_id,next,serde_json::to_string(&redo)?])?;
        tx.commit()?;
        Ok(())
    }

    fn document_state(
        &self,
        photo: &str,
        base: &str,
        settings: &ffi::FfiEditSettings,
    ) -> Result<State> {
        let record = if base.is_empty() {
            None
        } else {
            Some(
                self.catalog
                    .recipe_commit(photo.parse::<PhotoId>()?, base.parse::<RecipeCommitId>()?)?
                    .context("document history Recipe anchor missing")?,
            )
        };
        let draft = decode_grade_stack_draft_recipe_v1(settings)?;
        Ok(State {
            base: base.into(),
            snapshot: grade_stack_recipe_v1_snapshot(
                &draft,
                record.as_ref().map(|r| r.commit.snapshot()),
            )?,
        })
    }

    pub(crate) fn document_undo(&self, photo_id: &str) -> Result<ffi::FfiDocumentUndo> {
        let mut result = ffi::FfiDocumentUndo {
            undo: Vec::new(),
            redo: Vec::new(),
        };
        let Some(db) = open(&self.cache_root)? else {
            return Ok(result);
        };
        let (mut cursor, redo) = position(&db, photo_id)?;
        let mut visited = std::collections::BTreeSet::new();
        while let Some(id) = cursor {
            ensure!(visited.insert(id), "document history contains a cycle");
            let (parent, key, before, after) = step(&db, photo_id, id)?;
            result.undo.push(self.history_entry(key, &before, &after)?);
            cursor = parent;
            if result.undo.len() == 256 {
                break;
            }
        }
        result.undo.reverse();
        let redo: Vec<i64> = serde_json::from_str(&redo)?;
        ensure!(
            redo.len() <= 100_000,
            "document redo history exceeds its limit"
        );
        for id in redo.iter().rev().take(256).rev() {
            let (_, key, before, after) = step(&db, photo_id, *id)?;
            result.redo.push(self.history_entry(key, &before, &after)?);
        }
        Ok(result)
    }

    fn history_entry(
        &self,
        key: String,
        before: &str,
        after: &str,
    ) -> Result<ffi::FfiDocumentHistoryEntry> {
        let before: State = serde_json::from_str(before)?;
        let after: State = serde_json::from_str(after)?;
        let decode = |state: &State| -> Result<ffi::FfiEditSettings> {
            state.snapshot.validate_for_commit()?;
            let mut draft = decode_grade_stack_draft_from_recipe_v1_snapshot(&state.snapshot)?;
            crate::recipe_lut_resources::relocate_draft_luts(
                &mut draft,
                self.cache_root.parent().context("document root missing")?,
            )?;
            encode_grade_stack_draft_recipe_v1(draft)
        };
        Ok(ffi::FfiDocumentHistoryEntry {
            key,
            before: decode(&before)?,
            after: decode(&after)?,
            before_base: before.base,
            after_base: after.base,
        })
    }

    pub(crate) fn document_history_page(&self, photo_id: &str, before: i64) -> Result<String> {
        ensure!(before >= 0, "invalid document history cursor");
        let Some(db) = open(&self.cache_root)? else {
            return Ok("{\"items\":[],\"nextCursor\":null}".into());
        };
        let (cursor, _) = position(&db, photo_id)?;
        let mut query=db.prepare("SELECT id,parent,action FROM steps WHERE photo=?1 AND (?2=0 OR id<?2) ORDER BY id DESC LIMIT 129")?;
        let rows = query.query_map(params![photo_id, before], |r| {
            Ok((
                r.get::<_, i64>(0)?,
                r.get::<_, Option<i64>>(1)?,
                r.get::<_, String>(2)?,
            ))
        })?;
        let mut values = Vec::new();
        for row in rows {
            let (id, parent, action) = row?;
            values.push(serde_json::json!({"id":id.to_string(),"parent":parent.map(|v|v.to_string()),"action":action,"current":cursor==Some(id)}));
        }
        let more = values.len() > 128;
        values.truncate(128);
        let next = if more {
            values.last().and_then(|v| v.get("id")).cloned()
        } else {
            None
        };
        Ok(serde_json::to_string(
            &serde_json::json!({"items":values,"nextCursor":next}),
        )?)
    }

    pub(crate) fn checkout_document_history(
        &self,
        photo_id: &str,
        id: i64,
    ) -> Result<ffi::FfiDocumentHistoryEntry> {
        let db = open(&self.cache_root)?.context("no document history")?;
        let (_, _, before, after) = step(&db, photo_id, id)?;
        self.history_entry(id.to_string(), &before, &after)
    }
}

fn open(cache: &Path) -> Result<Option<Connection>> {
    let root = cache.parent().context("document runtime root missing")?;
    if !root.join(".shadow-document-session").is_file() {
        return Ok(None);
    }
    let directory = root.join("document-history");
    std::fs::create_dir_all(&directory)?;
    let db = Connection::open(directory.join("history.sqlite"))?;
    db.busy_timeout(std::time::Duration::from_secs(5))?;
    db.execute_batch("PRAGMA trusted_schema=OFF; CREATE TABLE IF NOT EXISTS steps(id INTEGER PRIMARY KEY,photo TEXT NOT NULL,parent INTEGER,action TEXT NOT NULL,before_state TEXT NOT NULL,after_state TEXT NOT NULL); CREATE TABLE IF NOT EXISTS cursors(photo TEXT PRIMARY KEY,step INTEGER,redo TEXT NOT NULL);")?;
    Ok(Some(db))
}

fn position(db: &Connection, photo: &str) -> Result<(Option<i64>, String)> {
    Ok(db
        .query_row(
            "SELECT step,redo FROM cursors WHERE photo=?1",
            [photo],
            |r| Ok((r.get(0)?, r.get(1)?)),
        )
        .optional()?
        .unwrap_or((None, "[]".into())))
}
fn step(db: &Connection, photo: &str, id: i64) -> Result<(Option<i64>, String, String, String)> {
    Ok(db.query_row(
        "SELECT parent,action,before_state,after_state FROM steps WHERE id=?1 AND photo=?2",
        params![id, photo],
        |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?, r.get(3)?)),
    )?)
}

pub(super) fn export_history(
    root: &Path,
    photo: &str,
    path: &Path,
    resources: &mut super::resources::ResourceClosure,
    source: &Path,
) -> Result<()> {
    let db = open(&root.join("cache"))?.context("not a document session")?;
    let (cursor, redo) = position(&db, photo)?;
    let mut output = std::fs::File::create(path)?;
    let header = serde_json::json!({"format":"dev.shadow.document-history","version":1,"photoId":photo,"cursor":cursor,"redo":serde_json::from_str::<Vec<i64>>(&redo)?});
    let mut bytes = serde_json::to_vec(&header)?;
    bytes.pop();
    output.write_all(&bytes)?;
    output.write_all(b",\"steps\":[")?;
    let mut query = db.prepare(
        "SELECT id,parent,action,before_state,after_state FROM steps WHERE photo=?1 ORDER BY id",
    )?;
    let mut rows = query.query([photo])?;
    let mut first = true;
    while let Some(row) = rows.next()? {
        let before: State = serde_json::from_str(&row.get::<_, String>(3)?)?;
        let after: State = serde_json::from_str(&row.get::<_, String>(4)?)?;
        before.snapshot.validate_for_commit()?;
        after.snapshot.validate_for_commit()?;
        resources.validate(&before.snapshot, source)?;
        resources.validate(&after.snapshot, source)?;
        if !first {
            output.write_all(b",")?;
        }
        first = false;
        serde_json::to_writer(
            &mut output,
            &serde_json::json!({"id":row.get::<_,i64>(0)?,"parent":row.get::<_,Option<i64>>(1)?,"action":row.get::<_,String>(2)?,"before":before,"after":after}),
        )?;
    }
    output.write_all(b"]}")?;
    output.sync_all()?;
    Ok(())
}

// A managed LUT's verified content identity is semantic; its historical host
// directory is only a locator. All other persisted fields remain exact.
fn equivalent_snapshot(a: &RecipeSnapshot, b: &RecipeSnapshot) -> Result<bool> {
    if a == b {
        return Ok(true);
    }
    fn normalize(value: &mut serde_json::Value) {
        match value {
            serde_json::Value::Object(object) => {
                let managed=object.get(shadow_domain::operation::LUT_RESOURCE_ID_PARAMETER_KEY)
                    .and_then(|value|serde_json::from_value::<shadow_domain::ParameterValue>(value.clone()).ok())
                    .is_some_and(|value|matches!(value,shadow_domain::ParameterValue::Text(ref id) if id.len()==64 && id.bytes().all(|v|v.is_ascii_hexdigit())));
                if managed {
                    object.remove(shadow_domain::operation::LUT_MANAGED_PATH_PARAMETER_KEY);
                }
                for child in object.values_mut() {
                    normalize(child);
                }
            }
            serde_json::Value::Array(array) => {
                for child in array {
                    normalize(child);
                }
            }
            _ => {}
        }
    }
    let mut left = serde_json::to_value(a)?;
    let mut right = serde_json::to_value(b)?;
    normalize(&mut left);
    normalize(&mut right);
    Ok(left == right)
}
