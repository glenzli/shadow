//! Photo-grid paging, counting, and facet queries.
//!
//! These reads share one validated filter-to-SQL projection so the grid, counts, and facets cannot
//! drift into subtly different Library semantics.

use std::fmt::Write as _;

use rusqlite::{params_from_iter, types::Value};
use shadow_domain::EntityId;

use crate::{Catalog, CatalogError};

use super::{
    LibraryFacetCursor, LibraryFacetKind, LibraryFacetPage, LibraryPhotoCursor,
    LibraryPhotoCursorValue, LibraryPhotoFilter, LibraryPhotoOrder, LibraryPhotoPage,
    MAX_LIBRARY_FACET_PAGE_SIZE, MAX_LIBRARY_PAGE_SIZE,
    model::validate_library_photo_filter,
    query_projection::library_photo_query_parts,
    rows::{read_library_facet, read_library_photo},
};
use crate::asset_registration::location_file_name_sort_key;

impl Catalog {
    /// Returns one bounded, photo-first Library grid page.
    ///
    /// The query deliberately starts with logical photos rather than imported
    /// folders. It selects an online original source representation (RAW or
    /// raster) and location only as an opening target, while metadata, likes,
    /// decisions, and album membership stay attached to the logical photo.
    /// When a logical photo has both kinds, RAW remains preferred so existing
    /// edit/development flows keep their source choice.
    ///
    /// Pagination is keyset-based: an ordinary scroll does not become slower
    /// as a catalog grows from thousands to millions of images.
    pub fn library_photo_page(
        &self,
        filter: &LibraryPhotoFilter,
        order: LibraryPhotoOrder,
        after: Option<&LibraryPhotoCursor>,
        requested_limit: usize,
    ) -> Result<LibraryPhotoPage, CatalogError> {
        validate_library_photo_filter(filter)?;
        let page_size = requested_limit.clamp(1, MAX_LIBRARY_PAGE_SIZE);
        let (from_sql, where_sql, filter_values) = library_photo_query_parts(filter);

        let mut page_sql = format!(
            "SELECT p.id, r.id, l.platform, l.native_path, l.display_path,
                    r.byte_len, r.modified_at_ms,
                    f.photo_id, f.captured_at_unix_seconds, f.capture_day,
                    f.camera_make, f.camera_model, f.lens_make, f.lens_model,
                    f.aperture_milli, f.focal_length_tenth_mm, f.iso_speed,
                    f.latitude_e7, f.longitude_e7, f.place_name,
                    f.indexed_representation_id, f.indexed_source_byte_len,
                    f.indexed_source_modified_at_ms, f.indexed_at_ms,
                    COALESCE(s.liked, 0), COALESCE(s.color_label, 'none'),
                    COALESCE(s.updated_at_ms, 0),
                    dc.head_sequence, de.after_flag, de.after_rating,
                    EXISTS (
                        SELECT 1 FROM recipe_refs edit_ref
                        WHERE edit_ref.photo_id = p.id
                          AND edit_ref.name = 'working'
                    )
             {from_sql} WHERE {where_sql}"
        );
        let mut page_values = filter_values;
        append_library_order(&mut page_sql, &mut page_values, order, after)?;
        page_values.push(Value::Integer(
            i64::try_from(page_size + 1).unwrap_or(i64::MAX),
        ));

        let mut statement = self.connection.prepare(&page_sql)?;
        let rows = statement.query_map(params_from_iter(page_values.iter()), read_library_photo)?;
        let mut items = rows.collect::<rusqlite::Result<Vec<_>>>()?;
        let has_more = items.len() > page_size;
        items.truncate(page_size);
        let next_cursor = if has_more {
            items.last().map(|last| LibraryPhotoCursor {
                value: match order {
                    LibraryPhotoOrder::CaptureTimeDescending
                    | LibraryPhotoOrder::CaptureTimeAscending => {
                        LibraryPhotoCursorValue::CaptureTime(
                            last.facts
                                .as_ref()
                                .and_then(|facts| facts.captured_at_unix_seconds),
                        )
                    }
                    LibraryPhotoOrder::FileNameAscending
                    | LibraryPhotoOrder::FileNameDescending => LibraryPhotoCursorValue::FileName(
                        location_file_name_sort_key(&last.location.display_path),
                    ),
                },
                photo_id: last.photo_id,
            })
        } else {
            None
        };
        Ok(LibraryPhotoPage { items, next_cursor })
    }

    /// Calculates an exact photo count for a settled Library filter.
    ///
    /// This is intentionally separate from [`Self::library_photo_page`]. A
    /// virtualized grid should fetch keyset pages immediately and schedule this
    /// potentially expensive aggregate only after the user stops changing
    /// facets.
    pub fn library_photo_count(&self, filter: &LibraryPhotoFilter) -> Result<u64, CatalogError> {
        validate_library_photo_filter(filter)?;
        let (from_sql, where_sql, values) = library_photo_query_parts(filter);
        let total: i64 = self.connection.query_row(
            &format!("SELECT COUNT(*) {from_sql} WHERE {where_sql}"),
            params_from_iter(values.iter()),
            |row| row.get(0),
        )?;
        u64::try_from(total).map_err(|error| CatalogError::InvalidLibraryQuery(error.to_string()))
    }

    /// Returns a bounded page of one dynamically composed Library facet.
    ///
    /// The selected dimension is intentionally removed from the incoming
    /// filter before grouping. This is the useful Lightroom-style behaviour:
    /// after selecting one camera, the Camera section still shows meaningful
    /// alternatives while all other active constraints remain in force.
    ///
    /// Facets are ordered by matching photo count, then a stable key. The
    /// aggregate runs only when the desktop explicitly opens or refreshes the
    /// facet browser; it is not part of the hot scroll path.
    pub fn library_facet_page(
        &self,
        filter: &LibraryPhotoFilter,
        kind: LibraryFacetKind,
        after: Option<&LibraryFacetCursor>,
        requested_limit: usize,
    ) -> Result<LibraryFacetPage, CatalogError> {
        validate_library_photo_filter(filter)?;
        if let Some(cursor) = after
            && (cursor.key.trim().is_empty() || cursor.key.len() > 512)
        {
            return Err(CatalogError::InvalidLibraryQuery(
                "facet cursor key must contain 1 through 512 characters".into(),
            ));
        }

        let page_size = requested_limit.clamp(1, MAX_LIBRARY_FACET_PAGE_SIZE);
        let facet_filter = filter_without_facet(filter, kind);
        let (from_sql, where_sql, mut values) = library_photo_query_parts(&facet_filter);
        let spec = library_facet_sql(kind);

        let mut sql = format!(
            "SELECT {key}, {label}, COUNT(*)\n             {from_sql}\n             WHERE {where_sql} AND {present}\n             GROUP BY {key}",
            key = spec.key,
            label = spec.label,
            present = spec.present,
        );
        if let Some(cursor) = after {
            let _ = write!(
                sql,
                " HAVING COUNT(*) < ? OR (COUNT(*) = ? AND {key} > ?)",
                key = spec.key,
            );
            let count = i64::try_from(cursor.photo_count).map_err(|error| {
                CatalogError::InvalidLibraryQuery(format!("facet cursor count is invalid: {error}"))
            })?;
            values.push(Value::Integer(count));
            values.push(Value::Integer(count));
            values.push(Value::Text(cursor.key.clone()));
        }
        let _ = write!(
            sql,
            " ORDER BY COUNT(*) DESC, {key} ASC LIMIT ?",
            key = spec.key,
        );
        values.push(Value::Integer(
            i64::try_from(page_size + 1).unwrap_or(i64::MAX),
        ));

        let mut statement = self.connection.prepare(&sql)?;
        let rows = statement.query_map(params_from_iter(values.iter()), read_library_facet)?;
        let mut items = rows.collect::<rusqlite::Result<Vec<_>>>()?;
        let has_more = items.len() > page_size;
        items.truncate(page_size);
        let next_cursor = if has_more {
            items.last().map(|last| LibraryFacetCursor {
                photo_count: last.photo_count,
                key: last.key.clone(),
            })
        } else {
            None
        };
        Ok(LibraryFacetPage { items, next_cursor })
    }
}

fn append_library_order(
    sql: &mut String,
    values: &mut Vec<Value>,
    order: LibraryPhotoOrder,
    after: Option<&LibraryPhotoCursor>,
) -> Result<(), CatalogError> {
    match order {
        LibraryPhotoOrder::CaptureTimeDescending | LibraryPhotoOrder::CaptureTimeAscending => {
            let descending = order == LibraryPhotoOrder::CaptureTimeDescending;
            if let Some(cursor) = after {
                let LibraryPhotoCursorValue::CaptureTime(captured_at) = cursor.value else {
                    return Err(CatalogError::InvalidLibraryQuery(
                        "capture-time order requires a capture-time cursor".into(),
                    ));
                };
                if let Some(captured_at) = captured_at {
                    let comparison = if descending { "<" } else { ">" };
                    sql.push_str(&format!(
                        " AND (f.captured_at_unix_seconds IS NULL
                                  OR f.captured_at_unix_seconds {comparison} ?
                                  OR (f.captured_at_unix_seconds = ? AND p.id {comparison} ?))"
                    ));
                    values.push(Value::Integer(captured_at));
                    values.push(Value::Integer(captured_at));
                    values.push(Value::Blob(cursor.photo_id.as_bytes().to_vec()));
                } else {
                    let comparison = if descending { "<" } else { ">" };
                    sql.push_str(&format!(
                        " AND f.captured_at_unix_seconds IS NULL AND p.id {comparison} ?"
                    ));
                    values.push(Value::Blob(cursor.photo_id.as_bytes().to_vec()));
                }
            }
            let direction = if descending { "DESC" } else { "ASC" };
            sql.push_str(&format!(
                " ORDER BY CASE WHEN f.captured_at_unix_seconds IS NULL THEN 1 ELSE 0 END,
                           f.captured_at_unix_seconds {direction}, p.id {direction}
                  LIMIT ?"
            ));
        }
        LibraryPhotoOrder::FileNameAscending | LibraryPhotoOrder::FileNameDescending => {
            let descending = order == LibraryPhotoOrder::FileNameDescending;
            if let Some(cursor) = after {
                let LibraryPhotoCursorValue::FileName(ref name) = cursor.value else {
                    return Err(CatalogError::InvalidLibraryQuery(
                        "file-name order requires a file-name cursor".into(),
                    ));
                };
                if name.is_empty() {
                    return Err(CatalogError::InvalidLibraryQuery(
                        "file-name cursor must not be empty".into(),
                    ));
                }
                let comparison = if descending { "<" } else { ">" };
                sql.push_str(&format!(
                    " AND (l.sort_name_key {comparison} ?
                              OR (l.sort_name_key = ? AND p.id {comparison} ?))"
                ));
                values.push(Value::Text(name.clone()));
                values.push(Value::Text(name.clone()));
                values.push(Value::Blob(cursor.photo_id.as_bytes().to_vec()));
            }
            let direction = if descending { "DESC" } else { "ASC" };
            sql.push_str(&format!(
                " ORDER BY l.sort_name_key {direction}, p.id {direction} LIMIT ?"
            ));
        }
    }
    Ok(())
}

struct LibraryFacetSql {
    key: &'static str,
    label: &'static str,
    present: &'static str,
}

fn library_facet_sql(kind: LibraryFacetKind) -> LibraryFacetSql {
    match kind {
        LibraryFacetKind::CaptureMonth => LibraryFacetSql {
            key: "substr(f.capture_day, 1, 7)",
            label: "substr(f.capture_day, 1, 7)",
            present: "f.capture_day <> ''",
        },
        LibraryFacetKind::Camera => LibraryFacetSql {
            key: "f.camera_key",
            label: "MIN(COALESCE(NULLIF(trim(f.camera_make || ' ' || f.camera_model), ''), f.camera_key))",
            present: "f.camera_key <> ''",
        },
        LibraryFacetKind::Lens => LibraryFacetSql {
            key: "f.lens_key",
            label: "MIN(COALESCE(NULLIF(trim(f.lens_make || ' ' || f.lens_model), ''), f.lens_key))",
            present: "f.lens_key <> ''",
        },
    }
}

fn filter_without_facet(filter: &LibraryPhotoFilter, kind: LibraryFacetKind) -> LibraryPhotoFilter {
    let mut result = filter.clone();
    match kind {
        LibraryFacetKind::CaptureMonth => result.capture_month = None,
        LibraryFacetKind::Camera => result.camera_key = None,
        LibraryFacetKind::Lens => result.lens_key = None,
    }
    result
}
