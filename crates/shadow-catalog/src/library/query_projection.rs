//! Shared projection from a validated Library filter to one photo-first SQL row set.
//!
//! Grid paging, facets, counts, and spatial browsing compile through this owner so a new Library
//! presentation cannot silently invent different album, decision, or source-selection semantics.

use rusqlite::types::Value;
use shadow_domain::EntityId;

use super::{
    LibraryPhotoFilter,
    model::{capture_month_bounds, normalize_query_key},
};

// This is one ordered compiler from the validated filter contract to SQL and
// bound values; splitting individual predicates would make their order drift.
#[allow(clippy::too_many_lines)]
pub(super) fn library_photo_query_parts(
    filter: &LibraryPhotoFilter,
) -> (String, String, Vec<Value>) {
    // Both correlated subqueries are backed by the current v1 catalog's
    // `(photo, kind, created)` and `(representation, status, created)` indexes.
    // This keeps one logical row per photo without requiring a directory-derived
    // materialized view. Original rasters are first-class Library sources; RAW
    // retains a stable preference only when both are attached to one logical photo.
    // A catalog with no source registry remains readable for legacy imports. As
    // soon as any durable source row exists, however, that registry is the
    // authoritative Library boundary: an unowned historical path must not
    // silently reappear after its folder was removed.
    let from_sql = "FROM photos p
         JOIN representations r ON r.id = (
             SELECT r2.id FROM representations r2
             WHERE r2.photo_id = p.id
               AND r2.kind IN ('original_raw', 'original_raster')
               AND EXISTS (
                   SELECT 1 FROM locations l2
                   WHERE l2.representation_id = r2.id AND l2.status = 'online'
                     AND (
                         EXISTS (
                             SELECT 1
                             FROM location_sources ownership
                             JOIN library_sources source
                               ON source.id = ownership.source_id
                              AND source.enabled = 1
                             WHERE ownership.location_id = l2.id
                         )
                         OR (
                             NOT EXISTS (SELECT 1 FROM library_sources)
                             AND NOT EXISTS (
                                 SELECT 1 FROM location_sources ownership
                                 WHERE ownership.location_id = l2.id
                             )
                         )
                     )
               )
             ORDER BY CASE r2.kind WHEN 'original_raw' THEN 0 ELSE 1 END,
                      r2.created_at_ms DESC, r2.id DESC
             LIMIT 1
         )
         JOIN locations l ON l.id = (
             SELECT l3.id FROM locations l3
             WHERE l3.representation_id = r.id AND l3.status = 'online'
               AND (
                   EXISTS (
                       SELECT 1
                       FROM location_sources ownership
                       JOIN library_sources source
                         ON source.id = ownership.source_id
                        AND source.enabled = 1
                       WHERE ownership.location_id = l3.id
                   )
                   OR (
                       NOT EXISTS (SELECT 1 FROM library_sources)
                       AND NOT EXISTS (
                           SELECT 1 FROM location_sources ownership
                           WHERE ownership.location_id = l3.id
                       )
                   )
               )
             ORDER BY l3.created_at_ms DESC, l3.id DESC
             LIMIT 1
         )
         LEFT JOIN photo_library_effective_facts f ON f.photo_id = p.id
         LEFT JOIN library_place_resolutions place
           ON place.latitude_e7 = f.latitude_e7
          AND place.longitude_e7 = f.longitude_e7
         LEFT JOIN photo_library_state s ON s.photo_id = p.id
         LEFT JOIN photo_decision_current dc ON dc.photo_id = p.id
         LEFT JOIN photo_decision_events de
           ON de.sequence = dc.head_sequence AND de.photo_id = p.id"
        .to_owned();
    let mut clauses = vec!["p.lifecycle_state = 'active'".to_owned()];
    let mut values = Vec::new();

    if let Some(range) = filter.capture_time {
        if let Some(start) = range.start_inclusive {
            clauses.push("f.captured_at_unix_seconds >= ?".to_owned());
            values.push(Value::Integer(start));
        }
        if let Some(end) = range.end_inclusive {
            clauses.push("f.captured_at_unix_seconds <= ?".to_owned());
            values.push(Value::Integer(end));
        }
    }
    if let Some(capture_month) = filter.capture_month.as_deref() {
        // Every public query entry validates the filter before reaching this
        // hot SQL-fragment builder, so an invalid month here would be an
        // internal call-order bug rather than user input.
        let (first_day, next_first_day) = capture_month_bounds(capture_month)
            .expect("validated Library capture month must have bounds");
        clauses.push("f.capture_day >= ? AND f.capture_day < ?".to_owned());
        values.push(Value::Text(first_day));
        values.push(Value::Text(next_first_day));
    }
    if let Some(camera_key) = filter.camera_key.as_deref() {
        clauses.push("f.camera_key = ?".to_owned());
        values.push(Value::Text(normalize_query_key(camera_key)));
    }
    if let Some(lens_key) = filter.lens_key.as_deref() {
        clauses.push("f.lens_key = ?".to_owned());
        values.push(Value::Text(normalize_query_key(lens_key)));
    }
    if let Some(country_key) = filter.country_key.as_deref() {
        clauses.push("place.country_key = ?".to_owned());
        values.push(Value::Text(normalize_query_key(country_key)));
    }
    if let Some(locality_key) = filter.locality_key.as_deref() {
        clauses.push("place.locality_key = ?".to_owned());
        values.push(Value::Text(normalize_query_key(locality_key)));
    }
    if !filter.living_place_rules.is_empty() {
        // Travel is meaningful only for photos whose location has resolved to
        // a stable locality. A time-bounded home also treats an unknown
        // capture day conservatively: without a date we cannot prove travel.
        clauses.push("COALESCE(place.locality_key, '') <> ''".to_owned());
        for rule in &filter.living_place_rules {
            let locality_key = normalize_query_key(&rule.locality_key);
            match (rule.start_month.as_deref(), rule.end_month.as_deref()) {
                (None, None) => {
                    clauses.push("place.locality_key <> ?".to_owned());
                    values.push(Value::Text(locality_key));
                }
                (start_month, end_month) => {
                    let mut interval_clauses = Vec::new();
                    let mut interval_values = Vec::new();
                    if let Some(start_month) = start_month {
                        let (first_day, _) = capture_month_bounds(start_month)
                            .expect("validated living-place start month must have bounds");
                        interval_clauses.push("f.capture_day >= ?");
                        interval_values.push(Value::Text(first_day));
                    }
                    if let Some(end_month) = end_month {
                        let (_, next_first_day) = capture_month_bounds(end_month)
                            .expect("validated living-place end month must have bounds");
                        interval_clauses.push("f.capture_day < ?");
                        interval_values.push(Value::Text(next_first_day));
                    }
                    clauses.push(format!(
                        "NOT (place.locality_key = ? AND (COALESCE(f.capture_day, '') = '' OR ({})))",
                        interval_clauses.join(" AND ")
                    ));
                    values.push(Value::Text(locality_key));
                    values.extend(interval_values);
                }
            }
        }
    }
    if let Some(range) = filter.aperture {
        if let Some(minimum) = range.minimum_milli {
            clauses.push("f.aperture_milli >= ?".to_owned());
            values.push(Value::Integer(i64::from(minimum)));
        }
        if let Some(maximum) = range.maximum_milli {
            clauses.push("f.aperture_milli <= ?".to_owned());
            values.push(Value::Integer(i64::from(maximum)));
        }
    }
    if let Some(liked) = filter.liked {
        clauses.push("COALESCE(s.liked, 0) = ?".to_owned());
        values.push(Value::Integer(i64::from(liked)));
    }
    if let Some(color_label) = filter.color_label.as_deref() {
        clauses.push("COALESCE(s.color_label, 'none') = ?".to_owned());
        values.push(Value::Text(color_label.trim().to_ascii_lowercase()));
    }
    if let Some(flag) = filter.flag {
        clauses.push("COALESCE(de.after_flag, 'unflagged') = ?".to_owned());
        values.push(Value::Text(flag.as_str().to_owned()));
    }
    if let Some(minimum_rating) = filter.minimum_rating {
        clauses.push("COALESCE(de.after_rating, 0) >= ?".to_owned());
        values.push(Value::Integer(i64::from(minimum_rating)));
    }
    if let Some(has_development_edits) = filter.has_development_edits {
        let exists_working_recipe = "EXISTS (
             SELECT 1 FROM recipe_refs edit_ref
             WHERE edit_ref.photo_id = p.id
               AND edit_ref.name = 'working'
         )";
        clauses.push(if has_development_edits {
            exists_working_recipe.to_owned()
        } else {
            format!("NOT {exists_working_recipe}")
        });
    }
    if let Some(album_id) = filter.album_id {
        clauses.push(
            "EXISTS (
                 SELECT 1 FROM library_album_memberships m
                 WHERE m.photo_id = p.id AND m.album_id = ?
             )"
            .to_owned(),
        );
        values.push(Value::Blob(album_id.as_bytes().to_vec()));
    }
    for keyword_id in &filter.keyword_ids_all {
        clauses.push(
            "EXISTS (
                 WITH RECURSIVE keyword_subtree(id) AS (
                     SELECT id FROM library_keywords WHERE id = ?
                     UNION ALL
                     SELECT child.id
                     FROM library_keywords child
                     JOIN keyword_subtree parent ON child.parent_id = parent.id
                 )
                 SELECT 1
                 FROM library_photo_keywords assignment
                 JOIN keyword_subtree ON keyword_subtree.id = assignment.keyword_id
                 WHERE assignment.photo_id = p.id
             )"
            .to_owned(),
        );
        values.push(Value::Blob(keyword_id.as_bytes().to_vec()));
    }
    for keyword_id in &filter.excluded_keyword_ids_any {
        clauses.push(
            "NOT EXISTS (
                 WITH RECURSIVE keyword_subtree(id) AS (
                     SELECT id FROM library_keywords WHERE id = ?
                     UNION ALL
                     SELECT child.id
                     FROM library_keywords child
                     JOIN keyword_subtree parent ON child.parent_id = parent.id
                 )
                 SELECT 1
                 FROM library_photo_keywords assignment
                 JOIN keyword_subtree ON keyword_subtree.id = assignment.keyword_id
                 WHERE assignment.photo_id = p.id
             )"
            .to_owned(),
        );
        values.push(Value::Blob(keyword_id.as_bytes().to_vec()));
    }
    (from_sql, clauses.join(" AND "), values)
}
