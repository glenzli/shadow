//! Bounded, durable correction history. Analysis refresh rebases these snapshots.
use super::StoredSnapshot;
use anyhow::{Result as AnyResult, bail};
use rusqlite::{Connection, OptionalExtension, Transaction, params};

const MAX_ENTRIES: usize = 20;
const MAX_BYTES: usize = 32 * 1024 * 1024;

pub(super) fn initialize(connection: &Connection) -> AnyResult<()> {
    connection.execute_batch("CREATE TABLE IF NOT EXISTS people_corrections (sequence INTEGER PRIMARY KEY AUTOINCREMENT, snapshot TEXT NOT NULL) STRICT;")?;
    Ok(())
}
pub(super) fn available(connection: &Connection) -> AnyResult<bool> {
    Ok(
        connection.query_row("SELECT EXISTS(SELECT 1 FROM people_corrections)", [], |r| {
            r.get(0)
        })?,
    )
}
pub(super) fn read_all(connection: &Connection) -> AnyResult<Vec<(i64, StoredSnapshot)>> {
    let mut query = connection
        .prepare("SELECT sequence, snapshot FROM people_corrections ORDER BY sequence DESC")?;
    query
        .query_map([], |r| Ok((r.get::<_, i64>(0)?, r.get::<_, String>(1)?)))?
        .map(|row| {
            let (id, json) = row?;
            Ok((id, serde_json::from_str(&json)?))
        })
        .collect()
}
pub(super) fn latest(connection: &Connection) -> AnyResult<Option<(i64, StoredSnapshot)>> {
    let row = connection
        .query_row(
            "SELECT sequence, snapshot FROM people_corrections ORDER BY sequence DESC LIMIT 1",
            [],
            |r| Ok((r.get::<_, i64>(0)?, r.get::<_, String>(1)?)),
        )
        .optional()?;
    row.map(|(id, json)| Ok((id, serde_json::from_str(&json)?)))
        .transpose()
}
pub(super) fn push(transaction: &Transaction<'_>, snapshot: &StoredSnapshot) -> AnyResult<()> {
    let json = serde_json::to_string(snapshot)?;
    // Do not perform an un-undoable correction if the bounded history cannot admit it.
    if json.len() > MAX_BYTES {
        bail!("people correction exceeds the history budget");
    }
    transaction.execute(
        "INSERT INTO people_corrections(snapshot) VALUES (?1)",
        [&json],
    )?;
    trim(transaction)
}
pub(super) fn replace(
    transaction: &Transaction<'_>,
    snapshots: &[(i64, StoredSnapshot)],
) -> AnyResult<()> {
    for (id, snapshot) in snapshots {
        transaction.execute(
            "UPDATE people_corrections SET snapshot = ?1 WHERE sequence = ?2",
            params![serde_json::to_string(snapshot)?, id],
        )?;
    }
    trim(transaction)
}
fn trim(transaction: &Transaction<'_>) -> AnyResult<()> {
    let mut query = transaction.prepare("SELECT sequence, length(CAST(snapshot AS BLOB)) FROM people_corrections ORDER BY sequence DESC")?;
    let sizes = query
        .query_map([], |r| {
            Ok((r.get::<_, i64>(0)?, r.get::<_, i64>(1)? as usize))
        })?
        .collect::<Result<Vec<_>, _>>()?;
    let mut bytes = 0_usize;
    for (index, (id, size)) in sizes.into_iter().enumerate() {
        bytes = bytes.saturating_add(size);
        if index >= MAX_ENTRIES || bytes > MAX_BYTES {
            remove(transaction, id)?;
        }
    }
    Ok(())
}
pub(super) fn remove(transaction: &Transaction<'_>, id: i64) -> AnyResult<()> {
    transaction.execute("DELETE FROM people_corrections WHERE sequence = ?1", [id])?;
    Ok(())
}
pub(super) fn clear(transaction: &Transaction<'_>) -> AnyResult<()> {
    transaction.execute("DELETE FROM people_corrections", [])?;
    Ok(())
}
