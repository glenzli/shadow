//! Repeatable, opt-in scale smoke for the photo-first Library catalog.
//!
//! This is intentionally a catalog-level tool rather than a desktop benchmark:
//! it proves that keyset pagination remains complete and bounded before a
//! windowed grid, source-health UI, or facet panel is allowed to assume that
//! behavior at large counts. The synthetic source paths are never opened.

use std::{
    collections::HashSet,
    env,
    ffi::OsString,
    fs, io,
    path::{Path, PathBuf},
    process,
    time::{Instant, SystemTime, UNIX_EPOCH},
};

use shadow_catalog::{
    CatalogActor, LibraryPhotoFacts, LibraryPhotoFilter, MAX_LIBRARY_PAGE_SIZE, RegisterAsset,
    RepresentationFingerprint, SetPhotoLibraryState, library_equipment_key,
};
use shadow_domain::{AssetLocation, PhotoId, Platform, RepresentationKind};

const DEFAULT_PHOTO_COUNT: usize = 10_000;
const DEFAULT_PAGE_SIZE: usize = 256;
const BASE_CAPTURE_TIME: i64 = 1_700_000_000;

#[derive(Debug, Clone, Copy)]
struct Config {
    photo_count: usize,
    page_size: usize,
    keep_catalog: bool,
}

/// Runs a synthetic Catalog workload without touching a user's real Library.
///
/// # Errors
///
/// Returns an I/O error when the temporary catalog cannot be created, an
/// argument is malformed, or a Catalog invariant fails during the workload.
pub(super) fn run(arguments: impl IntoIterator<Item = OsString>) -> io::Result<()> {
    let Some(config) = parse_config(arguments)? else {
        return Ok(());
    };
    let scratch_root = scratch_root()?;
    fs::create_dir_all(&scratch_root)?;

    println!(
        "library-scale smoke: photos={} page-size={} root={}",
        config.photo_count,
        config.page_size,
        scratch_root.display()
    );
    let result = run_in(&scratch_root, config);
    if config.keep_catalog {
        println!(
            "library-scale smoke: retained catalog at {}",
            scratch_root.display()
        );
    } else {
        fs::remove_dir_all(&scratch_root)?;
    }
    result
}

fn parse_config(arguments: impl IntoIterator<Item = OsString>) -> io::Result<Option<Config>> {
    let mut config = Config {
        photo_count: DEFAULT_PHOTO_COUNT,
        page_size: DEFAULT_PAGE_SIZE,
        keep_catalog: false,
    };
    let mut arguments = arguments.into_iter();
    while let Some(argument) = arguments.next() {
        match argument.to_string_lossy().as_ref() {
            "--photos" => {
                config.photo_count = parse_positive_usize(arguments.next(), "--photos")?;
            }
            "--page-size" => {
                config.page_size = parse_positive_usize(arguments.next(), "--page-size")?;
            }
            "--keep" => config.keep_catalog = true,
            "--help" | "-h" => {
                println!(
                    "cargo xtask library-scale-smoke [--photos N] [--page-size N] [--keep]\n\
                     \n\
                     Builds a synthetic, file-backed Catalog and verifies bounded keyset paging.\n\
                     Defaults: --photos {DEFAULT_PHOTO_COUNT} --page-size {DEFAULT_PAGE_SIZE}.\n\
                     Use --photos 1000000 only for a deliberate long-running scale check."
                );
                return Ok(None);
            }
            value => {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidInput,
                    format!("unknown library-scale-smoke option {value:?}"),
                ));
            }
        }
    }
    if config.page_size > MAX_LIBRARY_PAGE_SIZE {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            format!("--page-size must not exceed the catalog bound of {MAX_LIBRARY_PAGE_SIZE}"),
        ));
    }
    Ok(Some(config))
}

fn parse_positive_usize(value: Option<OsString>, option: &str) -> io::Result<usize> {
    let Some(value) = value else {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            format!("{option} requires a positive integer"),
        ));
    };
    let parsed = value
        .to_string_lossy()
        .parse::<usize>()
        .map_err(|error| io::Error::new(io::ErrorKind::InvalidInput, error))?;
    if parsed == 0 {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            format!("{option} requires a positive integer"),
        ));
    }
    Ok(parsed)
}

fn scratch_root() -> io::Result<PathBuf> {
    let timestamp = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_err(io::Error::other)?
        .as_millis();
    Ok(env::temp_dir().join(format!(
        "shadow-library-scale-{}-{timestamp}",
        process::id()
    )))
}

fn run_in(scratch_root: &Path, config: Config) -> io::Result<()> {
    let catalog_path = scratch_root.join("catalog.sqlite");
    let actor = CatalogActor::spawn(&catalog_path).map_err(io::Error::other)?;
    let catalog = actor.handle();
    let insert_started = Instant::now();

    for index in 0..config.photo_count {
        let record = register_synthetic_photo(&catalog, index)?;
        if index.is_multiple_of(3) {
            catalog
                .set_photo_library_state(&SetPhotoLibraryState {
                    photo_id: record.photo_id,
                    liked: true,
                    color_label: if index.is_multiple_of(5) {
                        "blue"
                    } else {
                        "none"
                    }
                    .to_owned(),
                    updated_at_ms: synthetic_now(index),
                })
                .map_err(io::Error::other)?;
        }
    }

    let insert_elapsed = insert_started.elapsed();
    let all_started = Instant::now();
    let all = walk_all_pages(&catalog, &LibraryPhotoFilter::default(), config.page_size)?;
    let all_elapsed = all_started.elapsed();
    if all.len() != config.photo_count {
        return Err(io::Error::other(format!(
            "keyset walk returned {} photos, expected {}",
            all.len(),
            config.photo_count
        )));
    }

    let filtered = LibraryPhotoFilter {
        camera_key: Some(library_equipment_key("Synthetic", "ScaleCam A")),
        liked: Some(true),
        color_label: Some("blue".to_owned()),
        ..LibraryPhotoFilter::default()
    };
    let expected_filtered_count = (0..config.photo_count)
        .filter(|index| {
            index.is_multiple_of(2) && index.is_multiple_of(3) && index.is_multiple_of(5)
        })
        .count();
    let filtered_started = Instant::now();
    let filtered_ids = walk_all_pages(&catalog, &filtered, config.page_size)?;
    let filtered_elapsed = filtered_started.elapsed();
    if filtered_ids.len() != expected_filtered_count {
        return Err(io::Error::other(format!(
            "filtered keyset walk returned {}, expected {expected_filtered_count}",
            filtered_ids.len()
        )));
    }
    let filtered_count = catalog
        .library_photo_count(&filtered)
        .map_err(io::Error::other)?;
    if usize::try_from(filtered_count).ok() != Some(expected_filtered_count) {
        return Err(io::Error::other(format!(
            "filtered count query returned {filtered_count}, expected {expected_filtered_count}"
        )));
    }

    actor.shutdown().map_err(io::Error::other)?;
    println!(
        "library-scale smoke passed: inserted={} ({insert_elapsed:.2?}), \
         all-pages={} ({all_elapsed:.2?}), filtered={} ({filtered_elapsed:.2?})",
        config.photo_count,
        all.len(),
        filtered_ids.len(),
    );
    Ok(())
}

fn register_synthetic_photo(
    catalog: &shadow_catalog::CatalogHandle,
    index: usize,
) -> io::Result<shadow_catalog::RegisteredAsset> {
    let source = RepresentationFingerprint {
        byte_len: 20_000_000_u64.saturating_add(u64::try_from(index).unwrap_or(u64::MAX)),
        modified_at_ms: Some(synthetic_now(index)),
    };
    let display_path = format!("/synthetic-library/{index:08}.dng");
    let record = catalog
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                display_path.as_bytes().to_vec(),
                display_path,
            ),
            byte_len: source.byte_len,
            modified_at_ms: source.modified_at_ms,
            now_ms: synthetic_now(index),
        })
        .map_err(io::Error::other)?;
    let camera_model = if index.is_multiple_of(2) {
        "ScaleCam A"
    } else {
        "ScaleCam B"
    };
    catalog
        .upsert_photo_library_facts(&LibraryPhotoFacts {
            photo_id: record.photo_id,
            captured_at_unix_seconds: Some(
                BASE_CAPTURE_TIME.saturating_add(i64::try_from(index).unwrap_or(i64::MAX)),
            ),
            capture_day: "2026-07-26".to_owned(),
            camera_make: "Synthetic".to_owned(),
            camera_model: camera_model.to_owned(),
            lens_make: "Synthetic".to_owned(),
            lens_model: "Scale Lens 50mm".to_owned(),
            aperture_milli: Some(2_800),
            focal_length_tenth_mm: Some(500),
            iso_speed: Some(100.0),
            latitude_e7: None,
            longitude_e7: None,
            place_name: String::new(),
            indexed_representation_id: Some(record.representation_id),
            indexed_source: Some(source),
            indexed_at_ms: synthetic_now(index),
        })
        .map_err(io::Error::other)?;
    Ok(record)
}

fn walk_all_pages(
    catalog: &shadow_catalog::CatalogHandle,
    filter: &LibraryPhotoFilter,
    page_size: usize,
) -> io::Result<Vec<PhotoId>> {
    let mut cursor = None;
    let mut ids = Vec::new();
    let mut seen = HashSet::new();
    loop {
        let page = catalog
            .library_photo_page(
                filter,
                shadow_catalog::LibraryPhotoOrder::CaptureTimeDescending,
                cursor.as_ref(),
                page_size,
            )
            .map_err(io::Error::other)?;
        if page.items.len() > page_size {
            return Err(io::Error::other("catalog page exceeded requested bound"));
        }
        for item in page.items {
            if !seen.insert(item.photo_id) {
                return Err(io::Error::other(format!(
                    "keyset walk returned duplicate photo {}",
                    item.photo_id
                )));
            }
            ids.push(item.photo_id);
        }
        let Some(next_cursor) = page.next_cursor else {
            break;
        };
        cursor = Some(next_cursor);
    }
    Ok(ids)
}

fn synthetic_now(index: usize) -> i64 {
    1_700_000_000_000_i64.saturating_add(i64::try_from(index).unwrap_or(i64::MAX))
}
