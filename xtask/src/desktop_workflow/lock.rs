use std::{
    env, io,
    path::{Path, PathBuf},
    process::Command,
};

use super::process;

pub(crate) struct CanonicalDebugLock {
    helper: PathBuf,
    path: PathBuf,
    token: Option<String>,
}

impl CanonicalDebugLock {
    pub(crate) fn acquire(repository_root: &Path, local_build_root: &Path) -> io::Result<Self> {
        let helper = repository_root.join("scripts/acquire_debug_promotion_lock.py");
        let path = local_build_root.join(".promote-debug-build.lock");
        let python = env::var_os("PYTHON").unwrap_or_else(|| "python3".into());
        let owner = env::var("SHADOW_CANONICAL_DEBUG_STEWARD")
            .unwrap_or_else(|_| format!("canonical-debug-xtask-{}", std::process::id()));
        let token = process::capture(
            Command::new(python)
                .arg(&helper)
                .arg("acquire")
                .arg(&path)
                .arg("--owner")
                .arg(owner),
            "acquire canonical debug promotion lock",
        )?;
        Ok(Self {
            helper,
            path,
            token: Some(token),
        })
    }

    fn release(&mut self) {
        let Some(token) = self.token.take() else {
            return;
        };
        let python = env::var_os("PYTHON").unwrap_or_else(|| "python3".into());
        let _ = Command::new(python)
            .arg(&self.helper)
            .arg("release")
            .arg(&self.path)
            .arg("--token")
            .arg(token)
            .status();
    }
}

impl Drop for CanonicalDebugLock {
    fn drop(&mut self) {
        self.release();
    }
}
