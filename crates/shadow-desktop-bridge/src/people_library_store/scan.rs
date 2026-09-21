//! Durable completed-input identities for incremental scans; no embeddings.
use anyhow::Result as AnyResult;
use rusqlite::{Connection, Transaction, params};
use shadow_core::{PeopleAnalysisInput, PeopleAnalysisSelection};

pub(super) fn initialize(connection: &Connection) -> AnyResult<()> {
    connection.execute_batch("CREATE TABLE IF NOT EXISTS people_analyzed_inputs (representation_id TEXT PRIMARY KEY NOT NULL, photo_id TEXT NOT NULL, source_revision TEXT NOT NULL) STRICT;")?;
    Ok(())
}
pub(super) fn selection(connection: &Connection) -> AnyResult<PeopleAnalysisSelection> {
    let mut query = connection
        .prepare("SELECT representation_id, source_revision FROM people_analyzed_inputs")?;
    let known_inputs = query
        .query_map([], |r| Ok((r.get(0)?, r.get(1)?)))?
        .collect::<Result<_, _>>()?;
    let mut anchors = connection.prepare(
        "SELECT MIN(photo_id) FROM people_occurrences GROUP BY person_id ORDER BY person_id",
    )?;
    let anchor_photo_ids = anchors
        .query_map([], |r| r.get(0))?
        .collect::<Result<_, _>>()?;
    Ok(PeopleAnalysisSelection {
        known_inputs,
        anchor_photo_ids,
    })
}
pub(super) fn record(
    transaction: &Transaction<'_>,
    inputs: &[PeopleAnalysisInput],
) -> AnyResult<()> {
    for input in inputs {
        transaction.execute("INSERT INTO people_analyzed_inputs (representation_id, photo_id, source_revision) VALUES (?1, ?2, ?3) ON CONFLICT(representation_id) DO UPDATE SET source_revision=excluded.source_revision, photo_id=excluded.photo_id", params![input.representation_id, input.photo_id, input.source_revision])?;
    }
    Ok(())
}
pub(super) fn count(connection: &Connection) -> AnyResult<u32> {
    Ok(connection.query_row(
        "SELECT COUNT(DISTINCT photo_id) FROM people_analyzed_inputs",
        [],
        |r| r.get(0),
    )?)
}
pub(super) fn clear(transaction: &Transaction<'_>) -> AnyResult<()> {
    transaction.execute("DELETE FROM people_analyzed_inputs", [])?;
    Ok(())
}

pub(super) fn checked_photos(
    connection: &rusqlite::Connection,
) -> anyhow::Result<std::collections::BTreeSet<String>> {
    let mut statement =
        connection.prepare("SELECT DISTINCT photo_id FROM people_analyzed_inputs")?;
    Ok(statement
        .query_map([], |row| row.get(0))?
        .collect::<Result<_, _>>()?)
}
