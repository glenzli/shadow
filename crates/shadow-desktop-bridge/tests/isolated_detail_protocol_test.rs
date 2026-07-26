// Keep the first child-detail protocol independently testable before the
// high-contention desktop facade claims it. The production module deliberately
// remains unregistered in `lib.rs` until its request scheduler owns the wiring.
#[path = "../src/isolated_detail_protocol.rs"]
mod isolated_detail_protocol;
