use shadow_catalog::LiveCachedArtifactBlob;

use super::*;

#[test]
fn partition_keeps_only_cache_owned_algorithm_and_reports_the_rest() {
    let partitioned = partition_catalog_blobs(vec![
        LiveCachedArtifactBlob {
            algorithm: "blake3-256".into(),
            digest: [1; 32],
        },
        LiveCachedArtifactBlob {
            algorithm: "future-cache-v2".into(),
            digest: [2; 32],
        },
    ]);
    assert_eq!(partitioned.catalog_live_blob_count, 2);
    assert_eq!(partitioned.retained_blake3.len(), 1);
    assert!(
        partitioned
            .retained_blake3
            .contains(&BlobDigest::from_bytes([1; 32]))
    );
    assert_eq!(partitioned.unsupported_algorithms, ["future-cache-v2"]);
}
