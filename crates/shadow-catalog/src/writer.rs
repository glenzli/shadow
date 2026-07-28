//! Single-owner Catalog actor composition.
//!
//! [`protocol`] defines the internal command contract, [`handle`] adapts the
//! public client API to that contract, and [`dispatch`] executes commands on
//! the connection-owning thread. Responsibility families use matching child
//! modules across all three layers. `evidence` owns human decisions and
//! explicit feedback; `edit_history` owns the connected per-photo Recipe and
//! Library-wide edit repository contracts; `import_journal` owns durable scan
//! and explicit-relocation transactions; `export_preset` owns named settings
//! history; `export_queue` owns durable job/item execution and recovery;
//! `source_identity` owns registration, source fingerprints, and exact identity;
//! `source_health` owns Library source inventory and missing-location review;
//! `library_facts` owns filterable metadata and its source provenance;
//! `library_collections` owns affinity state, albums, and membership;
//! `library_browse` owns photo-first pages, counts, and bounded facets;
//! `decode_snapshot` owns provider observations and output-freshness queries;
//! `cached_artifact` owns content-addressed visual references and reachability;
//! `technical_observation` owns exact visual-quality evidence revisions;
//! `review_projection` owns photo/source/visual Review read models.

use std::{
    path::Path,
    sync::mpsc::{self, Receiver, Sender, SyncSender},
    thread::{self, JoinHandle},
};

use crate::{Catalog, CatalogError, CatalogStats};

mod dispatch;
mod handle;
mod protocol;
#[cfg(test)]
mod tests;

use dispatch::run_actor;
use protocol::Message;

#[derive(Debug)]
pub struct CatalogActor {
    handle: CatalogHandle,
    join_handle: Option<JoinHandle<()>>,
}

#[derive(Debug, Clone)]
pub struct CatalogHandle {
    sender: Sender<Message>,
}

impl CatalogActor {
    /// Starts a dedicated thread that exclusively owns the catalog connection.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the thread cannot start or the catalog cannot
    /// be opened and initialized.
    pub fn spawn(path: &Path) -> Result<Self, CatalogError> {
        let path = path.to_path_buf();
        Self::spawn_with(move || Catalog::open(&path))
    }

    #[cfg(test)]
    fn spawn_in_memory() -> Result<Self, CatalogError> {
        Self::spawn_with(Catalog::open_in_memory)
    }

    fn spawn_with(
        open_catalog: impl FnOnce() -> Result<Catalog, CatalogError> + Send + 'static,
    ) -> Result<Self, CatalogError> {
        let (sender, receiver) = mpsc::channel();
        let (ready_sender, ready_receiver) = mpsc::sync_channel(0);
        let join_handle = thread::Builder::new()
            .name("shadow-catalog-writer".to_owned())
            .spawn(move || match open_catalog() {
                Ok(catalog) => {
                    let _ = ready_sender.send(Ok(()));
                    run_actor(catalog, &receiver);
                }
                Err(error) => {
                    let _ = ready_sender.send(Err(error));
                }
            })
            .map_err(CatalogError::ActorStart)?;

        match ready_receiver.recv() {
            Ok(Ok(())) => Ok(Self {
                handle: CatalogHandle { sender },
                join_handle: Some(join_handle),
            }),
            Ok(Err(error)) => {
                let _ = join_handle.join();
                Err(error)
            }
            Err(_) => {
                let _ = join_handle.join();
                Err(CatalogError::ActorUnavailable)
            }
        }
    }

    pub fn handle(&self) -> CatalogHandle {
        self.handle.clone()
    }

    /// Stops the writer after all previously submitted commands.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the actor stopped unexpectedly or panicked.
    pub fn shutdown(mut self) -> Result<(), CatalogError> {
        self.stop_and_join()
    }

    fn stop_and_join(&mut self) -> Result<(), CatalogError> {
        let Some(join_handle) = self.join_handle.take() else {
            return Ok(());
        };
        let (response_sender, response_receiver) = mpsc::sync_channel(0);
        self.handle
            .sender
            .send(Message::Shutdown(response_sender))
            .map_err(|_| CatalogError::ActorUnavailable)?;
        response_receiver
            .recv()
            .map_err(|_| CatalogError::ActorUnavailable)?;
        join_handle.join().map_err(|_| CatalogError::ActorPanicked)
    }
}

impl Drop for CatalogActor {
    fn drop(&mut self) {
        let _ = self.stop_and_join();
    }
}
