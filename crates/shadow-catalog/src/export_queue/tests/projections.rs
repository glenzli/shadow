use shadow_domain::EntityId;

use crate::{Catalog, ExportItemId, NewExportItem};

use super::queue_fixtures::{enqueue_items, output, seeded_item};

#[test]
fn indexed_item_lookup_and_progress_do_not_materialize_the_parent_job() {
    let mut catalog = Catalog::open_in_memory().expect("catalog");
    let first = seeded_item(&mut catalog);
    let second = NewExportItem {
        output: output("/exports/second.jpg"),
        ..first.clone()
    };
    let job = enqueue_items(&mut catalog, vec![first, second], 3);
    let claimed = catalog
        .claim_next_export_item(4)
        .expect("claim")
        .expect("one item");

    assert_eq!(
        catalog.export_item(claimed.id).expect("indexed item"),
        Some(claimed.clone())
    );
    assert!(
        catalog
            .export_item(ExportItemId::new_v7())
            .expect("missing lookup")
            .is_none()
    );
    let progress = catalog.export_job_progress(job.id).expect("progress");
    assert_eq!(progress.total_item_count, 2);
    assert_eq!(progress.queued_item_count, 1);
    assert_eq!(progress.preparing_item_count, 1);
    assert_eq!(progress.active_item_count(), 1);
    assert_eq!(progress.resumable_item_count(), 1);
}
