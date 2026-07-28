use std::{
    env, fs, io,
    path::{Path, PathBuf},
    time::{SystemTime, UNIX_EPOCH},
};

pub(super) struct AcceptanceSession {
    data_root: PathBuf,
}

impl AcceptanceSession {
    pub(super) fn create() -> io::Result<Self> {
        let timestamp = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map_err(io::Error::other)?
            .as_millis();
        let data_root = env::temp_dir().join(format!(
            "shadow-daily-use-{}-{timestamp}",
            std::process::id()
        ));
        fs::create_dir_all(&data_root)?;
        Ok(Self { data_root })
    }

    pub(super) fn data_root(&self) -> &Path {
        &self.data_root
    }

    pub(super) fn remove(self) -> io::Result<()> {
        fs::remove_dir_all(self.data_root)
    }
}
