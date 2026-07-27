//! Catalog connection lifecycle, schema initialization, and aggregate statistics.

use std::{path::Path, time::Duration};

use rusqlite::Connection;
use shadow_domain::LocationStatus;

use crate::{
    CatalogError,
    row_codec::{count_rows, non_negative_count},
    schema_v1,
};

#[derive(Debug)]
pub struct Catalog {
    pub(crate) connection: Connection,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Default)]
pub struct CatalogStats {
    pub photos: u64,
    pub representations: u64,
    pub locations: u64,
    pub locations_needing_revalidation: u64,
}

impl Catalog {
    /// Opens or creates a file-backed catalog using the one current development
    /// schema. A catalog from an earlier development shape is rejected rather
    /// than migrated.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when `SQLite` cannot open, configure, or initialize
    /// the catalog.
    pub fn open(path: &Path) -> Result<Self, CatalogError> {
        let mut connection = Connection::open(path)?;
        configure_connection(&connection, true)?;
        schema_v1::initialize(&mut connection)?;
        Ok(Self { connection })
    }

    /// Opens an isolated in-memory catalog, primarily for tests.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when `SQLite` cannot initialize the
    /// in-memory database.
    pub fn open_in_memory() -> Result<Self, CatalogError> {
        let mut connection = Connection::open_in_memory()?;
        configure_connection(&connection, false)?;
        schema_v1::initialize(&mut connection)?;
        Ok(Self { connection })
    }

    /// Returns the active catalog schema version.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the schema state cannot be queried.
    pub fn schema_version(&self) -> Result<i64, CatalogError> {
        schema_v1::current_version(&self.connection).map_err(Into::into)
    }

    /// Returns persisted entity counts and the revalidation backlog.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if any count query fails or returns an invalid
    /// negative value.
    pub fn stats(&self) -> Result<CatalogStats, CatalogError> {
        let locations_needing_revalidation = self.connection.query_row(
            "SELECT COUNT(*) FROM locations WHERE status = ?1",
            [LocationStatus::NeedsRevalidation.as_str()],
            |row| row.get(0),
        )?;

        Ok(CatalogStats {
            photos: count_rows(&self.connection, "photos")?,
            representations: count_rows(&self.connection, "representations")?,
            locations: count_rows(&self.connection, "locations")?,
            locations_needing_revalidation: non_negative_count(locations_needing_revalidation)?,
        })
    }
}

pub(crate) fn configure_connection(
    connection: &Connection,
    file_backed: bool,
) -> rusqlite::Result<()> {
    connection.busy_timeout(Duration::from_secs(5))?;
    connection.execute_batch(
        "PRAGMA foreign_keys = ON;
         PRAGMA temp_store = MEMORY;",
    )?;

    if file_backed {
        connection.execute_batch(
            "PRAGMA journal_mode = WAL;
             PRAGMA synchronous = NORMAL;",
        )?;
    }

    Ok(())
}
