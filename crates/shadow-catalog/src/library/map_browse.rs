//! Bounded spatial aggregation for the Library map presentation.
//!
//! This owner deliberately knows nothing about map tiles or geocoding providers. It applies the
//! same photo-first filter as the grid, restricts it to a geographic viewport, and returns at most
//! one compact cluster per requested screen cell.

use rusqlite::{params_from_iter, types::Value};
use shadow_domain::{PhotoId, RepresentationId};

use crate::{Catalog, CatalogError, row_codec::read_id};

use super::{
    LibraryPhotoFilter, model::validate_library_photo_filter,
    query_projection::library_photo_query_parts,
};

/// Maximum number of columns or rows accepted for one spatial aggregation.
pub const MAX_LIBRARY_MAP_GRID_AXIS: u16 = 128;

/// Maximum number of spatial cells one request may materialize.
pub const MAX_LIBRARY_MAP_CELLS: usize = 4_096;

/// Geographic bounds currently visible in the Library map.
///
/// A west longitude greater than the east longitude denotes a viewport that crosses the
/// antimeridian. A full-world viewport uses -180° through +180°.
#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub struct LibraryMapViewport {
    pub south_latitude_e7: i32,
    pub west_longitude_e7: i32,
    pub north_latitude_e7: i32,
    pub east_longitude_e7: i32,
}

/// Screen-space aggregation resolution for one map query.
#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub struct LibraryMapGrid {
    pub columns: u16,
    pub rows: u16,
}

/// One visible map marker or cluster.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct LibraryMapCluster {
    pub cell_x: u16,
    pub cell_y: u16,
    pub latitude_e7: i32,
    pub longitude_e7: i32,
    pub photo_count: u64,
    /// Present only when this cell contains exactly one logical photo.
    pub single_photo_id: Option<PhotoId>,
    /// Present only with [`Self::single_photo_id`].
    pub single_representation_id: Option<RepresentationId>,
    /// Online opening target for a singleton. Empty for aggregated clusters.
    pub single_source_display_path: String,
}

/// Bounded spatial result for a settled filter and viewport.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct LibraryMapSnapshot {
    pub clusters: Vec<LibraryMapCluster>,
    pub photo_count: u64,
}

impl Catalog {
    /// Aggregates filtered, geotagged photos into a bounded viewport grid.
    ///
    /// This query does not contact a tile, search, or geocoding service. Coordinates come from
    /// Shadow's effective metadata projection, including manual and GPX corrections.
    // Keep SQL construction, positional bindings, and result decoding together;
    // their order is one audited spatial-query contract.
    #[allow(clippy::too_many_lines)]
    pub fn library_map_snapshot(
        &self,
        filter: &LibraryPhotoFilter,
        viewport: LibraryMapViewport,
        grid: LibraryMapGrid,
    ) -> Result<LibraryMapSnapshot, CatalogError> {
        validate_library_photo_filter(filter)?;
        validate_map_request(viewport, grid)?;

        let (from_sql, where_sql, filter_values) = library_photo_query_parts(filter);
        let crosses_antimeridian = viewport.west_longitude_e7 > viewport.east_longitude_e7;
        let normalized_east = if crosses_antimeridian {
            i64::from(viewport.east_longitude_e7) + 3_600_000_000_i64
        } else {
            i64::from(viewport.east_longitude_e7)
        };
        let normalized_west = i64::from(viewport.west_longitude_e7);
        let longitude_span = normalized_east - normalized_west;
        let latitude_span =
            i64::from(viewport.north_latitude_e7) - i64::from(viewport.south_latitude_e7);
        let longitude_predicate = if crosses_antimeridian {
            "(f.longitude_e7 >= ? OR f.longitude_e7 <= ?)"
        } else {
            "f.longitude_e7 BETWEEN ? AND ?"
        };

        let sql = format!(
            "WITH visible AS (
                 SELECT p.id AS photo_id,
                        r.id AS representation_id,
                        l.display_path AS display_path,
                        f.latitude_e7 AS latitude_e7,
                        f.longitude_e7 AS longitude_e7,
                        CASE WHEN ? AND f.longitude_e7 < ?
                             THEN f.longitude_e7 + 3600000000
                             ELSE f.longitude_e7
                        END AS normalized_longitude
                 {from_sql}
                 WHERE {where_sql}
                   AND f.latitude_e7 BETWEEN ? AND ?
                   AND {longitude_predicate}
             ),
             binned AS (
                 SELECT photo_id,
                        representation_id,
                        display_path,
                        latitude_e7,
                        longitude_e7,
                        normalized_longitude,
                        MIN(? - 1, ((normalized_longitude - ?) * ?) / ?) AS cell_x,
                        MIN(? - 1, ((? - latitude_e7) * ?) / ?) AS cell_y
                 FROM visible
             )
             SELECT cell_x,
                    cell_y,
                    CAST(ROUND(AVG(latitude_e7)) AS INTEGER),
                    CAST(ROUND(AVG(normalized_longitude)) AS INTEGER),
                    COUNT(*),
                    CASE WHEN COUNT(*) = 1 THEN MIN(photo_id) END,
                    CASE WHEN COUNT(*) = 1 THEN MIN(representation_id) END,
                    CASE WHEN COUNT(*) = 1 THEN MIN(display_path) ELSE '' END
             FROM binned
             GROUP BY cell_x, cell_y
             ORDER BY cell_y, cell_x"
        );

        // Positional parameters follow SQL text order: the normalized-longitude
        // projection appears before the shared filter predicates.
        let mut values = vec![
            Value::Integer(i64::from(crosses_antimeridian)),
            Value::Integer(normalized_west),
        ];
        values.extend(filter_values);
        values.extend([
            Value::Integer(i64::from(viewport.south_latitude_e7)),
            Value::Integer(i64::from(viewport.north_latitude_e7)),
            Value::Integer(i64::from(viewport.west_longitude_e7)),
            Value::Integer(i64::from(viewport.east_longitude_e7)),
            Value::Integer(i64::from(grid.columns)),
            Value::Integer(normalized_west),
            Value::Integer(i64::from(grid.columns)),
            Value::Integer(longitude_span),
            Value::Integer(i64::from(grid.rows)),
            Value::Integer(i64::from(viewport.north_latitude_e7)),
            Value::Integer(i64::from(grid.rows)),
            Value::Integer(latitude_span),
        ]);

        let mut statement = self.connection.prepare(&sql)?;
        let rows = statement.query_map(params_from_iter(values.iter()), |row| {
            let normalized_longitude: i64 = row.get(3)?;
            let wrapped_longitude = if normalized_longitude > 1_800_000_000 {
                normalized_longitude - 3_600_000_000_i64
            } else {
                normalized_longitude
            };
            let photo_count: i64 = row.get(4)?;
            Ok(LibraryMapCluster {
                cell_x: u16::try_from(row.get::<_, i64>(0)?).map_err(|error| {
                    rusqlite::Error::FromSqlConversionFailure(
                        0,
                        rusqlite::types::Type::Integer,
                        Box::new(error),
                    )
                })?,
                cell_y: u16::try_from(row.get::<_, i64>(1)?).map_err(|error| {
                    rusqlite::Error::FromSqlConversionFailure(
                        1,
                        rusqlite::types::Type::Integer,
                        Box::new(error),
                    )
                })?,
                latitude_e7: i32::try_from(row.get::<_, i64>(2)?).map_err(|error| {
                    rusqlite::Error::FromSqlConversionFailure(
                        2,
                        rusqlite::types::Type::Integer,
                        Box::new(error),
                    )
                })?,
                longitude_e7: i32::try_from(wrapped_longitude).map_err(|error| {
                    rusqlite::Error::FromSqlConversionFailure(
                        3,
                        rusqlite::types::Type::Integer,
                        Box::new(error),
                    )
                })?,
                photo_count: u64::try_from(photo_count).map_err(|error| {
                    rusqlite::Error::FromSqlConversionFailure(
                        4,
                        rusqlite::types::Type::Integer,
                        Box::new(error),
                    )
                })?,
                single_photo_id: read_optional_id(row, 5)?,
                single_representation_id: read_optional_id(row, 6)?,
                single_source_display_path: row.get(7)?,
            })
        })?;
        let clusters = rows.collect::<rusqlite::Result<Vec<_>>>()?;
        let photo_count = clusters
            .iter()
            .try_fold(0_u64, |total, cluster| {
                total.checked_add(cluster.photo_count)
            })
            .ok_or_else(|| {
                CatalogError::InvalidLibraryQuery("map photo count overflowed u64".into())
            })?;
        Ok(LibraryMapSnapshot {
            clusters,
            photo_count,
        })
    }
}

fn read_optional_id<I: shadow_domain::EntityId>(
    row: &rusqlite::Row<'_>,
    index: usize,
) -> rusqlite::Result<Option<I>> {
    if matches!(row.get_ref(index)?, rusqlite::types::ValueRef::Null) {
        Ok(None)
    } else {
        read_id(row, index).map(Some)
    }
}

fn validate_map_request(
    viewport: LibraryMapViewport,
    grid: LibraryMapGrid,
) -> Result<(), CatalogError> {
    if !(-900_000_000..=900_000_000).contains(&viewport.south_latitude_e7)
        || !(-900_000_000..=900_000_000).contains(&viewport.north_latitude_e7)
        || viewport.south_latitude_e7 >= viewport.north_latitude_e7
    {
        return Err(CatalogError::InvalidLibraryQuery(
            "map viewport must have increasing valid latitude bounds".into(),
        ));
    }
    if !(-1_800_000_000..=1_800_000_000).contains(&viewport.west_longitude_e7)
        || !(-1_800_000_000..=1_800_000_000).contains(&viewport.east_longitude_e7)
        || viewport.west_longitude_e7 == viewport.east_longitude_e7
    {
        return Err(CatalogError::InvalidLibraryQuery(
            "map viewport must have distinct valid longitude bounds".into(),
        ));
    }
    if grid.columns == 0
        || grid.rows == 0
        || grid.columns > MAX_LIBRARY_MAP_GRID_AXIS
        || grid.rows > MAX_LIBRARY_MAP_GRID_AXIS
        || usize::from(grid.columns) * usize::from(grid.rows) > MAX_LIBRARY_MAP_CELLS
    {
        return Err(CatalogError::InvalidLibraryQuery(format!(
            "map grid must contain 1 through {MAX_LIBRARY_MAP_CELLS} cells with no axis above \
             {MAX_LIBRARY_MAP_GRID_AXIS}"
        )));
    }
    Ok(())
}

#[cfg(test)]
mod tests;
