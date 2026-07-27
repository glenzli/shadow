//! Client-side `CatalogHandle` request adapters.

use super::*;

mod edit_history;
mod evidence;
mod export_preset;
mod export_queue;
mod import_journal;
mod source_health;
mod source_identity;

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
