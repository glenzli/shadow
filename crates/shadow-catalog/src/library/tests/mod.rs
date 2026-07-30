//! Owner-local contracts for the photo-first Library.
//!
//! Follow the production ownership map: [`browse`] covers grid queries and facets,
//! [`collections`] covers Library state and albums, [`keywords`] covers semantic organization,
//! [`facts`] covers indexed metadata, and [`sources`] covers durable source identity and scan
//! reconciliation.

mod asset_registration_fixture;
mod browse;
mod collections;
mod facts;
mod keywords;
mod library_fact_fixture;
mod metadata_overrides;
mod sources;
