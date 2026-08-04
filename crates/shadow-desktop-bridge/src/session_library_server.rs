use std::{net::SocketAddr, path::PathBuf, str::FromStr};

use anyhow::{Context, Result as AnyResult};
use shadow_library_sharing::AuthorizationToken;

use crate::{DesktopSession, LibraryServerSnapshot, LibraryServerStartRequest, ffi};

impl DesktopSession {
    pub(crate) fn library_server_snapshot(&self) -> AnyResult<ffi::FfiLibraryServerSnapshot> {
        self.library_server.snapshot().map(project_snapshot)
    }

    pub(crate) fn start_library_server(
        &self,
        config: &ffi::FfiLibraryServerConfig,
    ) -> AnyResult<ffi::FfiLibraryServerSnapshot> {
        let bind_address = SocketAddr::from_str(&config.bind_address)
            .with_context(|| format!("parse Library server address {}", config.bind_address))?;
        let authorization = AuthorizationToken::parse(config.authorization.clone())?;
        let request = LibraryServerStartRequest {
            bind_address,
            authorization,
            display_name: config.display_name.clone(),
            share_roots: config.share_roots.iter().map(PathBuf::from).collect(),
            serves_originals: config.serves_originals,
        };
        self.library_server.start(request).map(project_snapshot)
    }

    pub(crate) fn stop_library_server(&self) -> AnyResult<ffi::FfiLibraryServerSnapshot> {
        self.library_server.stop().map(project_snapshot)
    }

    pub(crate) fn reset_library_server_cache(&self) -> AnyResult<ffi::FfiLibraryServerSnapshot> {
        self.library_server.reset_cache().map(project_snapshot)
    }
}

fn project_snapshot(snapshot: LibraryServerSnapshot) -> ffi::FfiLibraryServerSnapshot {
    ffi::FfiLibraryServerSnapshot {
        running: snapshot.running,
        local_address: snapshot
            .local_address
            .map_or_else(String::new, |address| address.to_string()),
        display_name: snapshot.display_name,
        provider_mode: snapshot.provider_mode,
        photo_count: snapshot.photo_count,
        cache_byte_len: snapshot.cache_byte_len,
        shared_root_count: snapshot.shared_root_count,
        serves_originals: snapshot.serves_originals,
    }
}
