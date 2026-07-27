//! Client-side `CatalogHandle` request adapters and the import-store bridge.

use super::*;

mod evidence;

impl CatalogHandle {
    /// Returns the migrated schema version.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the writer is unavailable or the query fails.
    pub fn schema_version(&self) -> Result<i64, CatalogError> {
        self.request(Message::SchemaVersion)
    }

    /// Returns current entity counts.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the writer is unavailable or the query fails.
    pub fn stats(&self) -> Result<CatalogStats, CatalogError> {
        self.request(Message::Stats)
    }

    /// Registers an asset outside an import journal.
    ///
    /// This is reserved for focused tools and tests. Production import should
    /// use [`CatalogStore::register_import_asset`] so journal state is atomic.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the writer is unavailable or registration
    /// fails.
    pub fn register_asset(&self, request: &RegisterAsset) -> Result<RegisteredAsset, CatalogError> {
        self.request(|response| Message::RegisterAsset(request.clone(), response))
    }

    /// Registers a location while preserving photo identity when a separately
    /// verified path-independent identity has already been recorded.
    pub fn register_asset_with_content_identity(
        &self,
        request: &RegisterAsset,
        identity: &ContentIdentity,
    ) -> Result<RegisteredAsset, CatalogError> {
        self.request(|response| {
            Message::RegisterAssetWithContentIdentity(request.clone(), identity.clone(), response)
        })
    }

    /// Stores a full-file, format-payload, or decoder-mosaic identity through
    /// the single writer. Expensive identity generation stays outside the
    /// actor; the actor only validates and persists its result.
    pub fn record_representation_content_identity(
        &self,
        request: &RecordRepresentationContentIdentity,
    ) -> Result<RecordRepresentationContentIdentityStatus, CatalogError> {
        self.request(|response| {
            Message::RecordRepresentationContentIdentity(Box::new(request.clone()), response)
        })
    }

    /// Finds the representation that owns one separately verified exact
    /// content identity through the Catalog actor.
    ///
    /// This is a read-only proof step for source relocation. It deliberately
    /// does not attach a path: import journaling must perform that state change
    /// atomically in a subsequent, explicit operation.
    pub fn relink_match(
        &self,
        identity: &ContentIdentity,
    ) -> Result<Option<RelinkMatch>, CatalogError> {
        self.request(|response| Message::RelinkMatch(identity.clone(), response))
    }

    /// Reads the exact historical location selected from one completed source
    /// scan before a user-confirmed reattach is allowed to hash a candidate.
    pub fn missing_source_relink_target(
        &self,
        scan_session_id: ImportSessionId,
        location_id: shadow_domain::LocationId,
    ) -> Result<Option<MissingSourceRelinkTarget>, CatalogError> {
        self.request(|response| {
            Message::MissingSourceRelinkTarget(scan_session_id, location_id, response)
        })
    }

    /// Atomically binds a freshly discovered location to an already verified
    /// representation identity and records the import-journal result through
    /// the single catalog writer.
    ///
    /// This is intentionally an explicit relocation operation: a normal scan
    /// may never infer a merge from names, timestamps, or file sizes alone.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the discovered entry is absent, the target
    /// path already exists, or the exact identity does not still belong to the
    /// declared representation.
    pub fn register_import_verified_relocation(
        &self,
        session_id: ImportSessionId,
        request: &RegisterAsset,
        expected_representation_id: RepresentationId,
        identity: &ContentIdentity,
    ) -> Result<RegisteredAsset, CatalogError> {
        self.request(|response| {
            Message::RegisterImportVerifiedRelocation(
                session_id,
                request.clone(),
                expected_representation_id,
                identity.clone(),
                response,
            )
        })
    }

    /// Starts an explicit relocation journal which intentionally has no
    /// Library discovery source.
    pub fn begin_relocation_session(
        &self,
        root: &AssetLocation,
        now_ms: i64,
    ) -> Result<ImportSessionId, CatalogError> {
        self.request(|response| Message::BeginRelocationSession(root.clone(), now_ms, response))
    }

    /// Updates the compact, indexed photo facts projection after metadata
    /// extraction. It does not persist full EXIF again.
    pub fn upsert_photo_library_facts(
        &self,
        facts: &LibraryPhotoFacts,
    ) -> Result<(), CatalogError> {
        self.request(|response| Message::UpsertPhotoLibraryFacts(Box::new(facts.clone()), response))
    }

    pub fn photo_library_facts(
        &self,
        photo_id: PhotoId,
    ) -> Result<Option<LibraryPhotoFacts>, CatalogError> {
        self.request(|response| Message::PhotoLibraryFacts(photo_id, response))
    }

    /// Sets an independent Library like/color state for a photo.
    pub fn set_photo_library_state(
        &self,
        state: &SetPhotoLibraryState,
    ) -> Result<(), CatalogError> {
        self.request(|response| Message::SetPhotoLibraryState(Box::new(state.clone()), response))
    }

    pub fn photo_library_state(
        &self,
        photo_id: PhotoId,
    ) -> Result<PhotoLibraryState, CatalogError> {
        self.request(|response| Message::PhotoLibraryState(photo_id, response))
    }

    pub fn create_library_album(
        &self,
        kind: AlbumKind,
        name: &str,
        query_json: Option<&str>,
        now_ms: i64,
    ) -> Result<AlbumRecord, CatalogError> {
        self.request(|response| {
            Message::CreateLibraryAlbum(
                kind,
                name.to_owned(),
                query_json.map(str::to_owned),
                now_ms,
                response,
            )
        })
    }

    /// Creates a smart album from the strict, version-one query contract.
    pub fn create_smart_library_album(
        &self,
        name: &str,
        query: &SmartAlbumQueryV1,
        now_ms: i64,
    ) -> Result<AlbumRecord, CatalogError> {
        self.request(|response| {
            Message::CreateSmartLibraryAlbum(name.to_owned(), query.clone(), now_ms, response)
        })
    }

    /// Renames a manual or smart album without changing its membership or query.
    pub fn rename_library_album(
        &self,
        album_id: CollectionId,
        name: &str,
        now_ms: i64,
    ) -> Result<AlbumRecord, CatalogError> {
        self.request(|response| {
            Message::RenameLibraryAlbum(album_id, name.to_owned(), now_ms, response)
        })
    }

    /// Replaces the executable query of one existing smart album.
    pub fn replace_smart_album_query(
        &self,
        album_id: CollectionId,
        query: &SmartAlbumQueryV1,
        now_ms: i64,
    ) -> Result<AlbumRecord, CatalogError> {
        self.request(|response| {
            Message::ReplaceSmartAlbumQuery(album_id, query.clone(), now_ms, response)
        })
    }

    /// Deletes one album and only that album's explicit memberships.
    pub fn delete_library_album(&self, album_id: CollectionId) -> Result<bool, CatalogError> {
        self.request(|response| Message::DeleteLibraryAlbum(album_id, response))
    }

    pub fn library_albums(&self) -> Result<Vec<AlbumRecord>, CatalogError> {
        self.request(Message::LibraryAlbums)
    }

    pub fn add_photo_to_album(
        &self,
        album_id: CollectionId,
        photo_id: PhotoId,
        sort_key: i64,
        now_ms: i64,
    ) -> Result<(), CatalogError> {
        self.request(|response| {
            Message::AddPhotoToAlbum(album_id, photo_id, sort_key, now_ms, response)
        })
    }

    pub fn remove_photo_from_album(
        &self,
        album_id: CollectionId,
        photo_id: PhotoId,
    ) -> Result<bool, CatalogError> {
        self.request(|response| Message::RemovePhotoFromAlbum(album_id, photo_id, response))
    }

    pub fn albums_for_photo(&self, photo_id: PhotoId) -> Result<Vec<AlbumRecord>, CatalogError> {
        self.request(|response| Message::AlbumsForPhoto(photo_id, response))
    }

    pub fn library_sources(&self) -> Result<Vec<LibrarySourceRecord>, CatalogError> {
        self.request(Message::LibrarySources)
    }

    /// Lists source-level scan evidence without mutating location status.
    pub fn library_source_health(&self) -> Result<Vec<LibrarySourceHealth>, CatalogError> {
        self.request(Message::LibrarySourceHealth)
    }

    /// Reads a bounded review page of locations not observed by one completed
    /// source scan. A `None` page means that the requested legacy import
    /// session had no durable Library source.
    pub fn missing_source_location_page(
        &self,
        scan_session_id: ImportSessionId,
        after: Option<&MissingSourceLocationCursor>,
        requested_limit: usize,
    ) -> Result<Option<MissingSourceLocationPage>, CatalogError> {
        self.request(|response| {
            Message::MissingSourceLocationPage(
                scan_session_id,
                after.copied(),
                requested_limit,
                response,
            )
        })
    }

    /// Reads a bounded photo-first Library page through the single catalog
    /// actor. The cursor is stable across folders being renamed or reorganized.
    pub fn library_photo_page(
        &self,
        filter: &LibraryPhotoFilter,
        after: Option<&LibraryPhotoCursor>,
        requested_limit: usize,
    ) -> Result<LibraryPhotoPage, CatalogError> {
        self.request(|response| {
            Message::LibraryPhotoPage(filter.clone(), after.cloned(), requested_limit, response)
        })
    }

    /// Reads one bounded, photo-first aggregation page for a single Library
    /// facet. Callers should schedule this only when the facet browser is
    /// visible or needs an explicit refresh; grid scrolling remains a page
    /// query and never triggers aggregate work.
    pub fn library_facet_page(
        &self,
        filter: &LibraryPhotoFilter,
        kind: LibraryFacetKind,
        after: Option<&LibraryFacetCursor>,
        requested_limit: usize,
    ) -> Result<LibraryFacetPage, CatalogError> {
        self.request(|response| {
            Message::LibraryFacetPage(
                filter.clone(),
                kind,
                after.cloned(),
                requested_limit,
                response,
            )
        })
    }

    /// Counts a settled Library filter through the actor. Grid scrolling uses
    /// `library_photo_page`; this explicit aggregate can be debounced.
    pub fn library_photo_count(&self, filter: &LibraryPhotoFilter) -> Result<u64, CatalogError> {
        self.request(|response| Message::LibraryPhotoCount(filter.clone(), response))
    }

    /// Resolves the executable, indexed filter behind one smart album.
    pub fn smart_album_filter(
        &self,
        album_id: CollectionId,
    ) -> Result<LibraryPhotoFilter, CatalogError> {
        self.request(|response| Message::SmartAlbumFilter(album_id, response))
    }

    /// Reads one bounded keyset page from a smart album.
    pub fn smart_album_photo_page(
        &self,
        album_id: CollectionId,
        after: Option<&LibraryPhotoCursor>,
        requested_limit: usize,
    ) -> Result<LibraryPhotoPage, CatalogError> {
        self.request(|response| {
            Message::SmartAlbumPhotoPage(album_id, after.cloned(), requested_limit, response)
        })
    }

    /// Counts one settled smart album through the actor.
    pub fn smart_album_photo_count(&self, album_id: CollectionId) -> Result<u64, CatalogError> {
        self.request(|response| Message::SmartAlbumPhotoCount(album_id, response))
    }

    /// Returns the current source fingerprint for a representation.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the writer is unavailable, the representation
    /// is absent, or the query fails.
    pub fn representation_fingerprint(
        &self,
        representation_id: RepresentationId,
    ) -> Result<RepresentationFingerprint, CatalogError> {
        self.request(|response| Message::RepresentationFingerprint(representation_id, response))
    }

    /// Records one decoder snapshot through the single catalog writer.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the writer is unavailable or persistence
    /// fails. A normal source race is reported in the returned status.
    pub fn record_decode_snapshot(
        &self,
        request: &RecordDecodeSnapshot,
    ) -> Result<RecordDecodeSnapshotStatus, CatalogError> {
        self.request(|response| Message::RecordDecodeSnapshot(Box::new(request.clone()), response))
    }

    /// Returns all current provider snapshots for a representation.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the writer is unavailable, the representation
    /// is absent, or persisted state cannot be read.
    pub fn decode_snapshots(
        &self,
        representation_id: RepresentationId,
    ) -> Result<Vec<DecodeSnapshotRecord>, CatalogError> {
        self.request(|response| Message::DecodeSnapshots(representation_id, response))
    }

    /// Reports whether a provider's required output is current for a source.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the writer is unavailable or the query fails.
    #[allow(clippy::too_many_arguments)]
    pub fn is_decode_output_current(
        &self,
        representation_id: RepresentationId,
        provider_id: &str,
        provider_version: &str,
        source: RepresentationFingerprint,
        require_cached_preview: bool,
        proxy_variant_key: &str,
        required_technical_preprocessing: Option<&str>,
    ) -> Result<bool, CatalogError> {
        self.request(|response| {
            Message::IsDecodeOutputCurrent(
                representation_id,
                provider_id.to_owned(),
                provider_version.to_owned(),
                source,
                require_cached_preview,
                proxy_variant_key.to_owned(),
                required_technical_preprocessing.map(str::to_owned),
                response,
            )
        })
    }

    /// Records a content-addressed cached artifact through the catalog writer.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the writer is unavailable or persistence
    /// fails. Source races are returned as a normal status.
    pub fn record_cached_artifact(
        &self,
        request: &RecordCachedArtifact,
    ) -> Result<RecordCachedArtifactStatus, CatalogError> {
        self.request(|response| Message::RecordCachedArtifact(Box::new(request.clone()), response))
    }

    /// Returns cached preview/proxy references for a representation.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the writer is unavailable or the query fails.
    pub fn cached_artifacts(
        &self,
        representation_id: RepresentationId,
    ) -> Result<Vec<CachedArtifactRecord>, CatalogError> {
        self.request(|response| Message::CachedArtifacts(representation_id, response))
    }

    /// Returns the shared Review-selected current visual through the Catalog
    /// writer so analysis and UI use one artifact-selection contract.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the writer is unavailable, the
    /// representation is absent, or persisted metadata is invalid.
    pub fn preferred_cached_artifact(
        &self,
        representation_id: RepresentationId,
    ) -> Result<Option<CachedArtifactRecord>, CatalogError> {
        self.request(|response| Message::PreferredCachedArtifact(representation_id, response))
    }

    /// Selects current cache artifacts for one bounded Library page through
    /// the single Catalog writer. The returned slots preserve request order,
    /// so callers can attach visuals without a per-thumbnail actor round-trip.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the writer is unavailable, a requested
    /// representation is absent, or persisted artifact metadata is invalid.
    pub fn preferred_cached_artifacts(
        &self,
        representation_ids: &[RepresentationId],
    ) -> Result<Vec<Option<CachedArtifactRecord>>, CatalogError> {
        self.request(|response| {
            Message::PreferredCachedArtifacts(representation_ids.to_vec(), response)
        })
    }

    /// Lists every cache blob that is still reachable from a current Catalog
    /// source/Recipe snapshot. Cache maintenance uses this with its own
    /// filesystem inventory to perform a conservative sweep.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the writer is unavailable or Catalog finds
    /// malformed persisted cache metadata.
    pub fn live_cached_artifact_blobs(&self) -> Result<Vec<LiveCachedArtifactBlob>, CatalogError> {
        self.request(Message::LiveCachedArtifactBlobs)
    }

    /// Invalidates an exact cache reference through the single Catalog writer.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the writer is unavailable or the conditional
    /// delete fails.
    pub fn invalidate_cached_artifact(
        &self,
        record: &CachedArtifactRecord,
    ) -> Result<InvalidateCachedArtifactStatus, CatalogError> {
        self.request(|response| {
            Message::InvalidateCachedArtifact(Box::new(record.clone()), response)
        })
    }

    /// Records one display-luma observation through the single Catalog writer.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the writer is unavailable or persistence
    /// fails. A normal source/artifact race is returned in the status.
    pub fn record_technical_observation(
        &self,
        request: &RecordTechnicalObservation,
    ) -> Result<RecordTechnicalObservationStatus, CatalogError> {
        self.request(|response| {
            Message::RecordTechnicalObservation(Box::new(request.clone()), response)
        })
    }

    /// Reads one exact, current technical observation through the Catalog actor.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the actor is unavailable or matching
    /// persisted state fails integrity validation.
    pub fn technical_observation(
        &self,
        representation_id: RepresentationId,
        expected_source: RepresentationFingerprint,
        expected_artifact: &crate::CachedArtifact,
        revision: &TechnicalObservationRevision,
    ) -> Result<Option<TechnicalObservationRecord>, CatalogError> {
        self.request(|response| {
            Message::TechnicalObservation(
                representation_id,
                expected_source,
                Box::new(expected_artifact.clone()),
                revision.clone(),
                response,
            )
        })
    }

    /// Returns one immutable, keyset-paginated Review-grid page through the
    /// Catalog actor.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the writer is unavailable or the query fails.
    pub fn review_page(
        &self,
        after: Option<&ReviewCursor>,
        limit: usize,
    ) -> Result<ReviewPageRecord, CatalogError> {
        self.request(|response| Message::ReviewPage(after.cloned(), limit, None, None, response))
    }

    /// Returns a Review page with summaries for one exact technical revision.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the writer is unavailable, the query fails,
    /// or a matching persisted observation fails integrity validation.
    pub fn review_page_with_technical(
        &self,
        after: Option<&ReviewCursor>,
        limit: usize,
        revision: &TechnicalObservationRevision,
    ) -> Result<ReviewPageRecord, CatalogError> {
        self.request(|response| {
            Message::ReviewPage(
                after.cloned(),
                limit,
                Some(revision.clone()),
                None,
                response,
            )
        })
    }

    /// Returns a Review page whose Recipe-preview candidates must match one
    /// exact generator implementation identity.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the writer is unavailable, the query fails,
    /// or a matching persisted observation fails integrity validation.
    pub fn review_page_with_technical_and_recipe_preview_generator(
        &self,
        after: Option<&ReviewCursor>,
        limit: usize,
        revision: &TechnicalObservationRevision,
        recipe_preview_generator: &CachedArtifactGeneratorIdentity,
    ) -> Result<ReviewPageRecord, CatalogError> {
        self.request(|response| {
            Message::ReviewPage(
                after.cloned(),
                limit,
                Some(revision.clone()),
                Some(recipe_preview_generator.clone()),
                response,
            )
        })
    }

    /// Returns the catalog-owned online RAW source for one photo.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the writer is unavailable or the query fails.
    pub fn review_source(
        &self,
        photo_id: PhotoId,
    ) -> Result<Option<ReviewItemRecord>, CatalogError> {
        self.request(|response| Message::ReviewSource(photo_id, None, response))
    }

    /// Returns one Review source with an exact current technical summary.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the actor is unavailable, the query fails,
    /// or a matching persisted observation fails integrity validation.
    pub fn review_source_with_technical(
        &self,
        photo_id: PhotoId,
        revision: &TechnicalObservationRevision,
    ) -> Result<Option<ReviewItemRecord>, CatalogError> {
        self.request(|response| Message::ReviewSource(photo_id, Some(revision.clone()), response))
    }

    /// Returns the online original source for one photo, preferring RAW and
    /// falling back to an original raster.
    ///
    /// Unlike [`Self::review_source`], this is for consumers with a
    /// source-neutral decoder route rather than legacy RAW-only callers.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the writer is unavailable or the query fails.
    pub fn photo_source(
        &self,
        photo_id: PhotoId,
    ) -> Result<Option<ReviewItemRecord>, CatalogError> {
        self.request(|response| Message::PhotoSource(photo_id, response))
    }

    /// Persists an immutable Recipe commit and optional ref move through the
    /// single Catalog writer.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the commit is invalid, a guarded ref has
    /// changed since it was read, or persistence fails.
    pub fn commit_recipe(
        &self,
        request: &CommitRecipe,
    ) -> Result<RecipeCommitRecord, CatalogError> {
        self.request(|response| Message::CommitRecipe(Box::new(request.clone()), response))
    }

    /// Lists every immutable Recipe commit owned by a photo.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the actor or persisted data is invalid.
    pub fn recipe_commits(
        &self,
        photo_id: PhotoId,
    ) -> Result<Vec<RecipeCommitRecord>, CatalogError> {
        self.request(|response| Message::RecipeCommits(photo_id, response))
    }

    /// Resolves one immutable Recipe commit by owner and id.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the actor or persisted data is invalid.
    pub fn recipe_commit(
        &self,
        photo_id: PhotoId,
        commit_id: RecipeCommitId,
    ) -> Result<Option<RecipeCommitRecord>, CatalogError> {
        self.request(|response| Message::RecipeCommit(photo_id, commit_id, response))
    }

    /// Resolves a named Recipe ref without changing it.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] for an invalid name or unavailable actor.
    pub fn recipe_ref(
        &self,
        photo_id: PhotoId,
        name: &str,
    ) -> Result<Option<RecipeRefRecord>, CatalogError> {
        self.request(|response| Message::RecipeRef(photo_id, name.to_owned(), response))
    }

    /// Moves a Recipe ref through the single Catalog writer.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the target does not belong to the photo or
    /// the write fails.
    pub fn set_recipe_ref(&self, request: &SetRecipeRef) -> Result<(), CatalogError> {
        self.request(|response| Message::SetRecipeRef(Box::new(request.clone()), response))
    }

    /// Discards all editable Recipe history for a single photo.
    ///
    /// This is intentionally not part of normal editing. The desktop uses it
    /// only after a user confirms that an obsolete development Recipe may be
    /// reset to the current fixed contract.
    pub fn discard_recipe_history(&self, photo_id: PhotoId) -> Result<usize, CatalogError> {
        self.request(|response| Message::DiscardRecipeHistory(photo_id, response))
    }

    /// Stores a topologically unordered pack of immutable edit objects through
    /// the Catalog's single writer transaction.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if an object is invalid, an edge target is
    /// absent, persisted content conflicts, or the actor is unavailable.
    pub fn store_edit_object_pack(
        &self,
        request: &EditObjectPackWrite,
    ) -> Result<StoreEditObjectPackResult, CatalogError> {
        self.request(|response| Message::StoreEditObjectPack(Box::new(request.clone()), response))
    }

    /// Reads and integrity-checks one content-addressed edit object.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if persisted bytes or indexed edges are corrupt
    /// or the actor is unavailable.
    pub fn edit_object(&self, id: EditObjectId) -> Result<Option<EditObjectRecord>, CatalogError> {
        self.request(|response| Message::EditObject(id, response))
    }

    /// Writes one Library-wide immutable edit commit and advances guarded refs
    /// in the same Catalog transaction.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] for missing roots/parents, stale ref heads,
    /// content collisions, or an unavailable actor.
    pub fn commit_edit_repository(
        &self,
        request: &CommitEditRepository,
    ) -> Result<EditRepositoryCommitRecord, CatalogError> {
        self.request(|response| Message::CommitEditRepository(Box::new(request.clone()), response))
    }

    /// Atomically publishes one per-photo compatibility commit and its
    /// Library-wide repository commit through the single writer.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if either commit/ref set is invalid or stale,
    /// persistence fails, or the actor is unavailable.
    pub fn commit_recipe_and_edit_repository(
        &self,
        request: &CommitRecipeAndEditRepository,
    ) -> Result<CommitRecipeAndEditRepositoryResult, CatalogError> {
        self.request(|response| {
            Message::CommitRecipeAndEditRepository(Box::new(request.clone()), response)
        })
    }

    /// Reads and integrity-checks one Library-wide edit commit.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when persisted data is invalid or the actor is
    /// unavailable.
    pub fn edit_repository_commit(
        &self,
        id: EditCommitId,
    ) -> Result<Option<EditRepositoryCommitRecord>, CatalogError> {
        self.request(|response| Message::EditRepositoryCommit(id, response))
    }

    /// Resolves one guarded Library-wide branch, named version, or tag.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] for an invalid name, invalid persisted data, or
    /// an unavailable actor.
    pub fn edit_repository_ref(
        &self,
        name: &str,
    ) -> Result<Option<EditRepositoryRefRecord>, CatalogError> {
        self.request(|response| Message::EditRepositoryRef(name.to_owned(), response))
    }

    /// Creates a named export preset and its first immutable settings revision
    /// through the single catalog writer.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the actor is unavailable or the preset is invalid.
    pub fn create_export_preset(
        &self,
        name: &str,
        settings_json: &str,
        now_ms: i64,
    ) -> Result<ExportPresetRevisionRecord, CatalogError> {
        self.request(|response| {
            Message::CreateExportPreset(name.to_owned(), settings_json.to_owned(), now_ms, response)
        })
    }

    /// Appends one immutable revision to an existing export preset.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the actor is unavailable or the revision is invalid.
    pub fn revise_export_preset(
        &self,
        preset_id: ExportPresetId,
        settings_json: &str,
        now_ms: i64,
    ) -> Result<ExportPresetRevisionRecord, CatalogError> {
        self.request(|response| {
            Message::ReviseExportPreset(preset_id, settings_json.to_owned(), now_ms, response)
        })
    }

    /// Lists named export presets through the catalog actor.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the actor is unavailable or the query fails.
    pub fn export_presets(&self) -> Result<Vec<ExportPresetRecord>, CatalogError> {
        self.request(Message::ExportPresets)
    }

    /// Lists all immutable revisions for one export preset.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the actor is unavailable or the preset is absent.
    pub fn export_preset_revisions(
        &self,
        preset_id: ExportPresetId,
    ) -> Result<Vec<ExportPresetRevisionRecord>, CatalogError> {
        self.request(|response| Message::ExportPresetRevisions(preset_id, response))
    }

    /// Atomically freezes one durable export job and all of its item snapshots.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the actor is unavailable or snapshots are invalid.
    pub fn enqueue_export_job(
        &self,
        request: &EnqueueExportJob,
    ) -> Result<ExportJobRecord, CatalogError> {
        self.request(|response| Message::EnqueueExportJob(Box::new(request.clone()), response))
    }

    /// Resolves one durable export job.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the actor is unavailable or persisted data is invalid.
    pub fn export_job(&self, job_id: ExportJobId) -> Result<Option<ExportJobRecord>, CatalogError> {
        self.request(|response| Message::ExportJob(job_id, response))
    }

    /// Lists a bounded task-center page of export jobs.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] for an unavailable actor or invalid page bound.
    pub fn export_jobs(&self, limit: usize) -> Result<Vec<ExportJobRecord>, CatalogError> {
        self.request(|response| Message::ExportJobs(limit, response))
    }

    /// Lists the individual immutable item snapshots for one export job.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the actor is unavailable or the job is absent.
    pub fn export_job_items(
        &self,
        job_id: ExportJobId,
    ) -> Result<Vec<ExportItemRecord>, CatalogError> {
        self.request(|response| Message::ExportJobItems(job_id, response))
    }

    /// Reads one export item by its primary-key identity without materializing
    /// the rest of its job. Workers use this on retry/completion paths so a
    /// huge batch remains O(1) per item.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the actor is unavailable or stored item
    /// data is malformed.
    pub fn export_item(
        &self,
        item_id: ExportItemId,
    ) -> Result<Option<ExportItemRecord>, CatalogError> {
        self.request(|response| Message::ExportItem(item_id, response))
    }

    /// Aggregates durable worker state for one job inside SQLite, without
    /// moving the job's individual item records through the actor.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the actor is unavailable, the job is
    /// absent, or its stored item state is malformed.
    pub fn export_job_progress(
        &self,
        job_id: ExportJobId,
    ) -> Result<ExportJobProgress, CatalogError> {
        self.request(|response| Message::ExportJobProgress(job_id, response))
    }

    /// Claims the next queued export item, atomically moving it to preparing.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the actor is unavailable or claiming fails.
    pub fn claim_next_export_item(
        &self,
        now_ms: i64,
    ) -> Result<Option<ExportItemRecord>, CatalogError> {
        self.request(|response| Message::ClaimNextExportItem(now_ms, response))
    }

    /// Advances one export item with a compare-and-swap state transition.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] for an unavailable actor or stale transition.
    pub fn advance_export_item(
        &self,
        request: &crate::AdvanceExportItem,
    ) -> Result<ExportItemRecord, CatalogError> {
        self.request(|response| Message::AdvanceExportItem(Box::new(request.clone()), response))
    }

    /// Returns the immutable receipt for a completed output item.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the actor is unavailable or receipt data is invalid.
    pub fn export_output_receipt(
        &self,
        item_id: ExportItemId,
    ) -> Result<Option<ExportOutputReceiptRecord>, CatalogError> {
        self.request(|response| Message::ExportOutputReceipt(item_id, response))
    }

    /// Cancels unfinished items in one job without deleting completed outputs.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the actor is unavailable or the job is absent.
    pub fn cancel_export_job(
        &self,
        job_id: ExportJobId,
        now_ms: i64,
    ) -> Result<ExportJobRecord, CatalogError> {
        self.request(|response| Message::CancelExportJob(job_id, now_ms, response))
    }

    /// Marks in-progress export work interrupted after a process restart.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the actor is unavailable or recovery fails.
    pub fn recover_interrupted_export_jobs(&self, now_ms: i64) -> Result<usize, CatalogError> {
        self.request(|response| Message::RecoverInterruptedExportJobs(now_ms, response))
    }

    /// Atomically makes every retryable interrupted item available at startup.
    ///
    /// The returned report uses global aggregate counts rather than task-center
    /// pages, so recovery stays complete for very large libraries and export
    /// batches.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the actor is unavailable or recovery
    /// cannot commit.
    pub fn recover_and_requeue_interrupted_export_items(
        &self,
        now_ms: i64,
    ) -> Result<ExportQueueRecovery, CatalogError> {
        self.request(|response| Message::RecoverAndRequeueInterruptedExportItems(now_ms, response))
    }

    /// Lists resumable import sessions.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the writer is unavailable or the query fails.
    pub fn unfinished_import_sessions(&self) -> Result<Vec<ImportSession>, CatalogError> {
        self.request(Message::UnfinishedImportSessions)
    }

    /// Returns the durable summary for one import session.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the writer is unavailable, the session is
    /// absent, or the query fails.
    pub fn import_session_summary(
        &self,
        id: ImportSessionId,
    ) -> Result<ImportSessionSummary, CatalogError> {
        self.request(|response| Message::ImportSessionSummary(id, response))
    }

    fn request<T: Send + 'static>(
        &self,
        message: impl FnOnce(SyncSender<Result<T, CatalogError>>) -> Message,
    ) -> Result<T, CatalogError> {
        let (response_sender, response_receiver) = mpsc::sync_channel(0);
        self.sender
            .send(message(response_sender))
            .map_err(|_| CatalogError::ActorUnavailable)?;
        response_receiver
            .recv()
            .map_err(|_| CatalogError::ActorUnavailable)?
    }
}

impl CatalogStore for CatalogHandle {
    fn begin_import_session(
        &mut self,
        root: &AssetLocation,
        now_ms: i64,
    ) -> Result<ImportSessionId, CatalogError> {
        self.request(|response| Message::BeginImportSession(root.clone(), now_ms, response))
    }

    fn resume_import_session(
        &mut self,
        id: ImportSessionId,
        now_ms: i64,
    ) -> Result<ImportSession, CatalogError> {
        self.request(|response| Message::ResumeImportSession(id, now_ms, response))
    }

    fn record_import_discovered(
        &mut self,
        session_id: ImportSessionId,
        request: &RegisterAsset,
    ) -> Result<(), CatalogError> {
        self.request(|response| {
            Message::RecordImportDiscovered(session_id, request.clone(), response)
        })
    }

    fn register_import_asset(
        &mut self,
        session_id: ImportSessionId,
        request: &RegisterAsset,
    ) -> Result<RegisteredAsset, CatalogError> {
        self.request(|response| Message::RegisterImportAsset(session_id, request.clone(), response))
    }

    fn register_import_verified_relocation(
        &mut self,
        session_id: ImportSessionId,
        request: &RegisterAsset,
        expected_representation_id: RepresentationId,
        identity: &ContentIdentity,
    ) -> Result<RegisteredAsset, CatalogError> {
        Self::register_import_verified_relocation(
            self,
            session_id,
            request,
            expected_representation_id,
            identity,
        )
    }

    fn record_import_issue(
        &mut self,
        session_id: ImportSessionId,
        location: &AssetLocation,
        message: &str,
        now_ms: i64,
    ) -> Result<(), CatalogError> {
        self.request(|response| {
            Message::RecordImportIssue(
                session_id,
                location.clone(),
                message.to_owned(),
                now_ms,
                response,
            )
        })
    }

    fn finish_import_session(
        &mut self,
        id: ImportSessionId,
        state: ImportSessionState,
        last_error: Option<&str>,
        now_ms: i64,
    ) -> Result<(), CatalogError> {
        self.request(|response| {
            Message::FinishImportSession(id, state, last_error.map(str::to_owned), now_ms, response)
        })
    }
}
