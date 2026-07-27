//! Shared decoding for SQLite entity identities and non-negative aggregate counts.

use rusqlite::{Connection, types::Type};
use shadow_domain::EntityId;
use uuid::Uuid;

pub(crate) fn read_id<I: EntityId>(row: &rusqlite::Row<'_>, index: usize) -> rusqlite::Result<I> {
    let bytes: Vec<u8> = row.get(index)?;
    let uuid = Uuid::from_slice(&bytes).map_err(|error| {
        rusqlite::Error::FromSqlConversionFailure(index, Type::Blob, Box::new(error))
    })?;
    Ok(I::from_uuid(uuid))
}

pub(crate) fn count_rows(connection: &Connection, table: &str) -> rusqlite::Result<u64> {
    let sql = match table {
        "photos" => "SELECT COUNT(*) FROM photos",
        "representations" => "SELECT COUNT(*) FROM representations",
        "locations" => "SELECT COUNT(*) FROM locations",
        _ => return Err(rusqlite::Error::InvalidQuery),
    };
    let count = connection.query_row(sql, [], |row| row.get(0))?;
    non_negative_count(count)
}

pub(crate) fn non_negative_count(count: i64) -> rusqlite::Result<u64> {
    u64::try_from(count).map_err(|error| {
        rusqlite::Error::FromSqlConversionFailure(0, Type::Integer, Box::new(error))
    })
}
