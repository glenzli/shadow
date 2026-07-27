use rusqlite::{Connection, OptionalExtension, Transaction, TransactionBehavior, params};
use shadow_domain::{
    EntityId, NewPhotoDecisionEvent, PhotoDecisionEvent, PhotoDecisionOrigin, PhotoDecisionState,
    PhotoFlag, PhotoId,
};

use crate::{Catalog, CatalogError, cache_artifact::digest, row_codec::read_id};

/// Hard upper bound for one photo's immutable decision-history query.
pub const MAX_PHOTO_DECISION_PAGE_SIZE: usize = 512;

/// One ascending, keyset-paginated page from a photo's decision ledger.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct PhotoDecisionPage {
    pub events: Vec<PhotoDecisionEvent>,
    pub has_more: bool,
}

struct StoredDecisionRow {
    sequence: i64,
    event_id: String,
    photo_id: PhotoId,
    occurred_at_ms: i64,
    origin: String,
    before_head_sequence: i64,
    before_flag: String,
    before_rating: i64,
    after_flag: String,
    after_rating: i64,
    json: String,
    digest: [u8; 32],
}

impl Catalog {
    /// Returns the current authoritative decision or the implicit default state.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the photo is absent or the current pointer
    /// does not resolve to one valid event owned by that photo.
    pub fn photo_decision_state(
        &self,
        photo_id: PhotoId,
    ) -> Result<PhotoDecisionState, CatalogError> {
        photo_decision_state_in_connection(&self.connection, photo_id)
    }

    /// Atomically appends one immutable human decision and advances its photo's
    /// current pointer with an expected-head compare-and-swap.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] for an invalid/no-op transition, unknown photo,
    /// duplicate event id, stale expected head, before-state disagreement, or
    /// persistence/integrity failure. No failure appends a partial event.
    pub fn append_photo_decision_event(
        &mut self,
        request: &NewPhotoDecisionEvent,
    ) -> Result<PhotoDecisionEvent, CatalogError> {
        request
            .validate()
            .map_err(|error| CatalogError::InvalidPhotoDecision(error.to_string()))?;

        let transaction = self
            .connection
            .transaction_with_behavior(TransactionBehavior::Immediate)?;
        ensure_event_id_absent(&transaction, &request.event_id)?;
        let current = photo_decision_state_in_connection(&transaction, request.photo_id)?;
        if current.head_sequence != request.expected_head_sequence {
            return Err(CatalogError::PhotoDecisionHeadMismatch {
                photo_id: request.photo_id,
                expected: request.expected_head_sequence,
                actual: current.head_sequence,
            });
        }
        if current.flag != request.before_flag || current.rating != request.before_rating {
            return Err(CatalogError::PhotoDecisionBeforeStateMismatch {
                photo_id: request.photo_id,
                head_sequence: current.head_sequence,
                expected_flag: request.before_flag,
                expected_rating: request.before_rating,
                actual_flag: current.flag,
                actual_rating: current.rating,
            });
        }

        let sequence = next_decision_sequence(&transaction)?;
        let event = request
            .clone()
            .with_sequence(sequence)
            .map_err(|error| CatalogError::InvalidPhotoDecision(error.to_string()))?;
        let event_json = canonical_json(&event)?;
        let event_digest = blake3::hash(event_json.as_bytes());
        let sequence_sql = sequence_to_sql(sequence)?;
        let before_head_sql = sequence_to_sql(event.before_head_sequence)?;
        transaction.execute(
            "INSERT INTO photo_decision_events(
                 sequence, event_id, photo_id, occurred_at_ms, origin,
                 before_head_sequence, before_flag, before_rating,
                 after_flag, after_rating, event_json, event_digest
             ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12)",
            params![
                sequence_sql,
                event.event_id,
                event.photo_id.as_bytes().as_slice(),
                event.occurred_at_unix_ms,
                event.origin.as_str(),
                before_head_sql,
                event.before_flag.as_str(),
                i64::from(event.before_rating),
                event.after_flag.as_str(),
                i64::from(event.after_rating),
                event_json,
                event_digest.as_bytes().as_slice(),
            ],
        )?;
        let moved = transaction.execute(
            "INSERT INTO photo_decision_current(photo_id, head_sequence)
             VALUES (?1, ?2)
             ON CONFLICT(photo_id) DO UPDATE SET head_sequence = excluded.head_sequence
             WHERE photo_decision_current.head_sequence = ?3",
            params![
                event.photo_id.as_bytes().as_slice(),
                sequence_sql,
                before_head_sql,
            ],
        )?;
        if moved != 1 {
            let actual = photo_decision_state_in_connection(&transaction, event.photo_id)?;
            return Err(CatalogError::PhotoDecisionHeadMismatch {
                photo_id: event.photo_id,
                expected: event.before_head_sequence,
                actual: actual.head_sequence,
            });
        }
        transaction.commit()?;
        Ok(event)
    }

    /// Reads a bounded ascending history page for exactly one photo.
    ///
    /// Every returned row is validated against its canonical JSON, digest, and
    /// indexed columns before it leaves Catalog.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] for an unknown photo, invalid page bound, or
    /// persisted integrity disagreement.
    pub fn photo_decision_events_after(
        &self,
        photo_id: PhotoId,
        after_sequence_exclusive: u64,
        limit: usize,
    ) -> Result<PhotoDecisionPage, CatalogError> {
        validate_page_limit(limit)?;
        ensure_photo_exists(&self.connection, photo_id)?;
        let after_sequence = sequence_to_sql(after_sequence_exclusive)?;
        let query_limit =
            i64::try_from(limit + 1).map_err(|_| CatalogError::InvalidPhotoDecisionPageLimit {
                limit,
                maximum: MAX_PHOTO_DECISION_PAGE_SIZE,
            })?;
        let mut statement = self.connection.prepare(
            "SELECT sequence, event_id, photo_id, occurred_at_ms, origin,
                    before_head_sequence, before_flag, before_rating,
                    after_flag, after_rating, event_json, event_digest
             FROM photo_decision_events
             WHERE photo_id = ?1 AND sequence > ?2
             ORDER BY sequence ASC
             LIMIT ?3",
        )?;
        let rows = statement.query_map(
            params![photo_id.as_bytes().as_slice(), after_sequence, query_limit],
            read_stored_decision_row,
        )?;

        let mut events = Vec::with_capacity(limit + 1);
        for row in rows {
            let row = row?;
            let event: PhotoDecisionEvent =
                serde_json::from_str(&row.json).map_err(CatalogError::PhotoDecisionJson)?;
            verify_decision_event(&event, &row)?;
            events.push(event);
        }
        let has_more = events.len() > limit;
        events.truncate(limit);
        Ok(PhotoDecisionPage { events, has_more })
    }
}

fn photo_decision_state_in_connection(
    connection: &Connection,
    photo_id: PhotoId,
) -> Result<PhotoDecisionState, CatalogError> {
    let head_sequence = connection
        .query_row(
            "SELECT c.head_sequence
             FROM photos p
             LEFT JOIN photo_decision_current c ON c.photo_id = p.id
             WHERE p.id = ?1",
            [photo_id.as_bytes().as_slice()],
            |row| row.get::<_, Option<i64>>(0),
        )
        .optional()?
        .ok_or(CatalogError::PhotoNotFound(photo_id))?;
    let Some(head_sequence) = head_sequence else {
        return Ok(PhotoDecisionState::default());
    };
    let row = connection
        .query_row(
            "SELECT sequence, event_id, photo_id, occurred_at_ms, origin,
                    before_head_sequence, before_flag, before_rating,
                    after_flag, after_rating, event_json, event_digest
             FROM photo_decision_events
             WHERE sequence = ?1 AND photo_id = ?2",
            params![head_sequence, photo_id.as_bytes().as_slice()],
            read_stored_decision_row,
        )
        .optional()?
        .ok_or(CatalogError::InvalidPersistedPhotoDecision(
            "current pointer does not resolve to an event owned by its photo",
        ))?;
    let event: PhotoDecisionEvent =
        serde_json::from_str(&row.json).map_err(CatalogError::PhotoDecisionJson)?;
    verify_decision_event(&event, &row)?;
    event.after_state().map_err(|_| {
        CatalogError::InvalidPersistedPhotoDecision("event payload failed domain validation")
    })
}

pub(crate) fn photo_decision_state_from_columns(
    head_sequence: Option<i64>,
    flag: Option<String>,
    rating: Option<i64>,
) -> Result<PhotoDecisionState, CatalogError> {
    match (head_sequence, flag, rating) {
        (None, None, None) => Ok(PhotoDecisionState::default()),
        (Some(head), Some(flag), Some(rating)) => PhotoDecisionState::new(
            sequence_from_sql(head)?,
            parse_flag(&flag)?,
            rating_from_sql(rating)?,
        )
        .map_err(|_| CatalogError::InvalidPersistedPhotoDecision("current head state is invalid")),
        _ => Err(CatalogError::InvalidPersistedPhotoDecision(
            "current pointer does not resolve to an event owned by its photo",
        )),
    }
}

fn ensure_photo_exists(connection: &Connection, photo_id: PhotoId) -> Result<(), CatalogError> {
    let exists = connection
        .query_row(
            "SELECT 1 FROM photos WHERE id = ?1",
            [photo_id.as_bytes().as_slice()],
            |_| Ok(()),
        )
        .optional()?
        .is_some();
    if exists {
        Ok(())
    } else {
        Err(CatalogError::PhotoNotFound(photo_id))
    }
}

fn ensure_event_id_absent(
    transaction: &Transaction<'_>,
    event_id: &str,
) -> Result<(), CatalogError> {
    let exists = transaction
        .query_row(
            "SELECT 1 FROM photo_decision_events WHERE event_id = ?1",
            [event_id],
            |_| Ok(()),
        )
        .optional()?
        .is_some();
    if exists {
        Err(CatalogError::PhotoDecisionEventAlreadyExists(
            event_id.to_owned(),
        ))
    } else {
        Ok(())
    }
}

fn next_decision_sequence(transaction: &Transaction<'_>) -> Result<u64, CatalogError> {
    let current: i64 = transaction.query_row(
        "SELECT COALESCE(MAX(sequence), 0) FROM photo_decision_events",
        [],
        |row| row.get(0),
    )?;
    let next = current
        .checked_add(1)
        .ok_or(CatalogError::PhotoDecisionSequenceExhausted)?;
    sequence_from_sql(next)
}

fn validate_page_limit(limit: usize) -> Result<(), CatalogError> {
    if (1..=MAX_PHOTO_DECISION_PAGE_SIZE).contains(&limit) {
        Ok(())
    } else {
        Err(CatalogError::InvalidPhotoDecisionPageLimit {
            limit,
            maximum: MAX_PHOTO_DECISION_PAGE_SIZE,
        })
    }
}

fn verify_decision_event(
    event: &PhotoDecisionEvent,
    stored: &StoredDecisionRow,
) -> Result<(), CatalogError> {
    event.validate().map_err(|_| {
        CatalogError::InvalidPersistedPhotoDecision("event payload failed domain validation")
    })?;
    let sequence = sequence_from_sql(stored.sequence)?;
    let before_head_sequence = sequence_from_sql(stored.before_head_sequence)?;
    let origin = parse_origin(&stored.origin)?;
    let before_flag = parse_flag(&stored.before_flag)?;
    let before_rating = rating_from_sql(stored.before_rating)?;
    let after_flag = parse_flag(&stored.after_flag)?;
    let after_rating = rating_from_sql(stored.after_rating)?;
    if event.sequence != sequence
        || event.event_id != stored.event_id
        || event.photo_id != stored.photo_id
        || event.occurred_at_unix_ms != stored.occurred_at_ms
        || event.origin != origin
        || event.before_head_sequence != before_head_sequence
        || event.before_flag != before_flag
        || event.before_rating != before_rating
        || event.after_flag != after_flag
        || event.after_rating != after_rating
    {
        return Err(CatalogError::InvalidPersistedPhotoDecision(
            "event JSON identity disagrees with indexed columns",
        ));
    }
    let canonical = canonical_json(event)?;
    if canonical != stored.json {
        return Err(CatalogError::InvalidPersistedPhotoDecision(
            "event JSON is not canonical",
        ));
    }
    if blake3::hash(stored.json.as_bytes()).as_bytes() != &stored.digest {
        return Err(CatalogError::InvalidPersistedPhotoDecision(
            "event JSON digest does not match",
        ));
    }
    Ok(())
}

fn read_stored_decision_row(row: &rusqlite::Row<'_>) -> rusqlite::Result<StoredDecisionRow> {
    Ok(StoredDecisionRow {
        sequence: row.get(0)?,
        event_id: row.get(1)?,
        photo_id: read_id(row, 2)?,
        occurred_at_ms: row.get(3)?,
        origin: row.get(4)?,
        before_head_sequence: row.get(5)?,
        before_flag: row.get(6)?,
        before_rating: row.get(7)?,
        after_flag: row.get(8)?,
        after_rating: row.get(9)?,
        json: row.get(10)?,
        digest: digest(row.get(11)?, 11)?,
    })
}

fn canonical_json<T: serde::Serialize>(value: &T) -> Result<String, CatalogError> {
    serde_json::to_string(value).map_err(CatalogError::PhotoDecisionJson)
}

fn parse_flag(value: &str) -> Result<PhotoFlag, CatalogError> {
    match value {
        "unflagged" => Ok(PhotoFlag::Unflagged),
        "picked" => Ok(PhotoFlag::Picked),
        "rejected" => Ok(PhotoFlag::Rejected),
        _ => Err(CatalogError::InvalidPersistedPhotoDecision(
            "event contains an unknown flag",
        )),
    }
}

fn parse_origin(value: &str) -> Result<PhotoDecisionOrigin, CatalogError> {
    match value {
        "human" => Ok(PhotoDecisionOrigin::Human),
        _ => Err(CatalogError::InvalidPersistedPhotoDecision(
            "event contains an unknown origin",
        )),
    }
}

fn rating_from_sql(value: i64) -> Result<u8, CatalogError> {
    let rating = u8::try_from(value).map_err(|_| {
        CatalogError::InvalidPersistedPhotoDecision("event rating is outside 0 through 5")
    })?;
    if rating > shadow_domain::MAX_PHOTO_RATING {
        Err(CatalogError::InvalidPersistedPhotoDecision(
            "event rating is outside 0 through 5",
        ))
    } else {
        Ok(rating)
    }
}

fn sequence_to_sql(sequence: u64) -> Result<i64, CatalogError> {
    i64::try_from(sequence).map_err(|_| CatalogError::PhotoDecisionSequenceExhausted)
}

fn sequence_from_sql(sequence: i64) -> Result<u64, CatalogError> {
    u64::try_from(sequence)
        .map_err(|_| CatalogError::InvalidPersistedPhotoDecision("event sequence is negative"))
}

#[cfg(test)]
mod tests {
    use shadow_domain::{AssetLocation, Platform, RepresentationKind};

    use super::*;
    use crate::{RegisterAsset, RegistrationStatus};

    fn register_photo(catalog: &mut Catalog, index: u8) -> PhotoId {
        let registered = catalog
            .register_asset(&RegisterAsset {
                kind: RepresentationKind::OriginalRaw,
                location: AssetLocation::new(
                    Platform::MacOs,
                    format!("/photos/decision-{index}.dng").into_bytes(),
                    format!("/photos/decision-{index}.dng"),
                ),
                byte_len: 42,
                modified_at_ms: Some(i64::from(index)),
                now_ms: 1_700_000_000_000 + i64::from(index),
            })
            .expect("register decision photo");
        assert_eq!(registered.status, RegistrationStatus::Inserted);
        registered.photo_id
    }

    fn request(
        event_id: &str,
        photo_id: PhotoId,
        before: PhotoDecisionState,
        after_flag: PhotoFlag,
        after_rating: u8,
    ) -> NewPhotoDecisionEvent {
        NewPhotoDecisionEvent {
            event_id: event_id.into(),
            photo_id,
            occurred_at_unix_ms: 1_700_000_001_000,
            origin: PhotoDecisionOrigin::Human,
            expected_head_sequence: before.head_sequence,
            before_flag: before.flag,
            before_rating: before.rating,
            after_flag,
            after_rating,
        }
    }

    fn catalog_with_decision(event_id: &str) -> (Catalog, PhotoId) {
        let mut catalog = Catalog::open_in_memory().expect("open decision catalog");
        let photo_id = register_photo(&mut catalog, 1);
        catalog
            .append_photo_decision_event(&request(
                event_id,
                photo_id,
                PhotoDecisionState::default(),
                PhotoFlag::Picked,
                3,
            ))
            .expect("append decision fixture");
        (catalog, photo_id)
    }

    #[test]
    fn initial_state_is_implicit_and_flag_rating_transitions_preserve_each_other() {
        let mut catalog = Catalog::open_in_memory().expect("open catalog");
        let photo_id = register_photo(&mut catalog, 1);
        assert_eq!(
            catalog
                .photo_decision_state(photo_id)
                .expect("default state"),
            PhotoDecisionState::default()
        );

        let picked = catalog
            .append_photo_decision_event(&request(
                "pick",
                photo_id,
                PhotoDecisionState::default(),
                PhotoFlag::Picked,
                0,
            ))
            .expect("pick photo");
        let picked_state = picked.after_state().unwrap();
        let rated = catalog
            .append_photo_decision_event(&request(
                "rate",
                photo_id,
                picked_state,
                PhotoFlag::Picked,
                5,
            ))
            .expect("rate without losing flag");
        let rated_state = rated.after_state().unwrap();
        let rejected = catalog
            .append_photo_decision_event(&request(
                "reject",
                photo_id,
                rated_state,
                PhotoFlag::Rejected,
                5,
            ))
            .expect("change flag without losing rating");
        let other_photo = register_photo(&mut catalog, 2);
        let other = catalog
            .append_photo_decision_event(&request(
                "other-photo",
                other_photo,
                PhotoDecisionState::default(),
                PhotoFlag::Picked,
                1,
            ))
            .expect("append globally sequenced decision for another photo");
        assert_eq!(
            (
                picked.sequence,
                rated.sequence,
                rejected.sequence,
                other.sequence
            ),
            (1, 2, 3, 4)
        );

        assert_eq!(
            catalog.photo_decision_state(photo_id).unwrap(),
            rejected.after_state().unwrap()
        );
        let page = catalog
            .photo_decision_events_after(photo_id, 0, 2)
            .expect("first history page");
        assert_eq!(page.events.len(), 2);
        assert!(page.has_more);
        let tail = catalog
            .photo_decision_events_after(photo_id, page.events[1].sequence, 2)
            .expect("tail history page");
        assert_eq!(tail.events, [rejected]);
        assert!(!tail.has_more);
    }

    #[test]
    fn noop_stale_head_and_before_mismatch_never_append() {
        let mut catalog = Catalog::open_in_memory().expect("open catalog");
        let photo_id = register_photo(&mut catalog, 1);
        let no_op = request(
            "noop",
            photo_id,
            PhotoDecisionState::default(),
            PhotoFlag::Unflagged,
            0,
        );
        assert!(matches!(
            catalog.append_photo_decision_event(&no_op),
            Err(CatalogError::InvalidPhotoDecision(_))
        ));

        let first = catalog
            .append_photo_decision_event(&request(
                "first",
                photo_id,
                PhotoDecisionState::default(),
                PhotoFlag::Picked,
                0,
            ))
            .expect("first decision");
        assert!(matches!(
            catalog.append_photo_decision_event(&request(
                "stale",
                photo_id,
                PhotoDecisionState::default(),
                PhotoFlag::Rejected,
                0,
            )),
            Err(CatalogError::PhotoDecisionHeadMismatch { .. })
        ));
        let mut wrong_before = first.after_state().unwrap();
        wrong_before.flag = PhotoFlag::Rejected;
        assert!(matches!(
            catalog.append_photo_decision_event(&request(
                "wrong-before",
                photo_id,
                wrong_before,
                PhotoFlag::Rejected,
                1,
            )),
            Err(CatalogError::PhotoDecisionBeforeStateMismatch { .. })
        ));
        let events = catalog
            .photo_decision_events_after(photo_id, 0, 10)
            .unwrap()
            .events;
        assert_eq!(events.as_slice(), std::slice::from_ref(&first));

        let unknown_photo = PhotoId::new_v7();
        assert!(matches!(
            catalog.append_photo_decision_event(&request(
                "unknown-photo",
                unknown_photo,
                PhotoDecisionState::default(),
                PhotoFlag::Picked,
                0,
            )),
            Err(CatalogError::PhotoNotFound(id)) if id == unknown_photo
        ));
        assert!(matches!(
            catalog.append_photo_decision_event(&request(
                "first",
                photo_id,
                first.after_state().unwrap(),
                PhotoFlag::Picked,
                1,
            )),
            Err(CatalogError::PhotoDecisionEventAlreadyExists(id)) if id == "first"
        ));
    }

    #[test]
    fn ledger_is_append_only_and_survives_reopen() {
        let root =
            std::env::temp_dir().join(format!("shadow-decision-ledger-{}", PhotoId::new_v7()));
        std::fs::create_dir_all(&root).expect("create decision fixture");
        let path = root.join("catalog.sqlite");
        let photo_id;
        let expected;
        {
            let mut catalog = Catalog::open(&path).expect("open catalog");
            photo_id = register_photo(&mut catalog, 1);
            expected = catalog
                .append_photo_decision_event(&request(
                    "durable",
                    photo_id,
                    PhotoDecisionState::default(),
                    PhotoFlag::Picked,
                    3,
                ))
                .expect("append durable decision");
            assert!(
                catalog
                    .connection
                    .execute(
                        "UPDATE photo_decision_events SET after_rating = 4 WHERE event_id = ?1",
                        ["durable"],
                    )
                    .is_err()
            );
            assert!(
                catalog
                    .connection
                    .execute(
                        "DELETE FROM photo_decision_events WHERE event_id = ?1",
                        ["durable"],
                    )
                    .is_err()
            );
        }
        let catalog = Catalog::open(&path).expect("reopen catalog");
        assert_eq!(
            catalog.photo_decision_state(photo_id).unwrap(),
            expected.after_state().unwrap()
        );
        assert_eq!(
            catalog
                .photo_decision_events_after(photo_id, 0, 10)
                .unwrap()
                .events,
            [expected]
        );
        drop(catalog);
        std::fs::remove_dir_all(root).expect("remove decision fixture");
    }

    #[test]
    fn history_and_current_getter_verify_digest_canonical_json_and_indexed_columns() {
        let (digest_catalog, digest_photo) = catalog_with_decision("digest-decision");
        digest_catalog
            .connection
            .execute_batch("DROP TRIGGER photo_decision_events_no_update")
            .expect("disable digest corruption guard");
        digest_catalog
            .connection
            .execute(
                "UPDATE photo_decision_events SET event_digest = zeroblob(32)
                 WHERE event_id = ?1",
                ["digest-decision"],
            )
            .expect("corrupt decision digest");
        assert!(matches!(
            digest_catalog.photo_decision_events_after(digest_photo, 0, 10),
            Err(CatalogError::InvalidPersistedPhotoDecision(
                "event JSON digest does not match"
            ))
        ));
        assert!(matches!(
            digest_catalog.photo_decision_state(digest_photo),
            Err(CatalogError::InvalidPersistedPhotoDecision(
                "event JSON digest does not match"
            ))
        ));

        let (canonical_catalog, canonical_photo) = catalog_with_decision("canonical-decision");
        canonical_catalog
            .connection
            .execute_batch("DROP TRIGGER photo_decision_events_no_update")
            .expect("disable canonical corruption guard");
        let canonical: String = canonical_catalog
            .connection
            .query_row(
                "SELECT event_json FROM photo_decision_events WHERE event_id = ?1",
                ["canonical-decision"],
                |row| row.get(0),
            )
            .expect("read canonical decision JSON");
        let non_canonical = format!(" {canonical}");
        let non_canonical_digest = blake3::hash(non_canonical.as_bytes());
        canonical_catalog
            .connection
            .execute(
                "UPDATE photo_decision_events SET event_json = ?1, event_digest = ?2
                 WHERE event_id = ?3",
                params![
                    non_canonical,
                    non_canonical_digest.as_bytes().as_slice(),
                    "canonical-decision",
                ],
            )
            .expect("corrupt decision canonical JSON");
        assert!(matches!(
            canonical_catalog.photo_decision_events_after(canonical_photo, 0, 10),
            Err(CatalogError::InvalidPersistedPhotoDecision(
                "event JSON is not canonical"
            ))
        ));

        let (indexed_catalog, indexed_photo) = catalog_with_decision("indexed-decision");
        indexed_catalog
            .connection
            .execute_batch("DROP TRIGGER photo_decision_events_no_update")
            .expect("disable indexed corruption guard");
        indexed_catalog
            .connection
            .execute(
                "UPDATE photo_decision_events SET occurred_at_ms = occurred_at_ms + 1
                 WHERE event_id = ?1",
                ["indexed-decision"],
            )
            .expect("corrupt indexed decision field");
        assert!(matches!(
            indexed_catalog.photo_decision_events_after(indexed_photo, 0, 10),
            Err(CatalogError::InvalidPersistedPhotoDecision(
                "event JSON identity disagrees with indexed columns"
            ))
        ));
    }
}
