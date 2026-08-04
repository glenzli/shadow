//! Startup negotiation for the isolated private decoder Provider Host.
//!
//! The child loads and validates the configured native provider graph. The caller receives only
//! bounded capability and cache-identity text, never a plugin path or SDK-owned object.

use std::path::Path;

use anyhow::{Result, bail};

use super::{
    helper_process::{
        HELPER_TIMEOUT, HelperExecution, HelperProcessOutput, execute_decode_helper,
        helper_stderr_suffix,
    },
    helper_wire_fields::decode_printable_identity_field,
};

const PROVIDER_HOST_PROTOCOL: &str = "shadow-provider-host-v1";
const PROVIDER_HOST_OPERATION: &str = "provider-inventory";

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct ProviderHostInventory {
    pub private_provider_available: bool,
    pub router_version: String,
}

/// Starts the selected helper and verifies its complete configured decoder graph.
///
/// # Errors
///
/// Returns an error when the helper cannot start, times out, rejects a provider module, or emits
/// a malformed inventory receipt.
pub fn inspect_provider_host(helper_path: &Path) -> Result<ProviderHostInventory> {
    match execute_decode_helper(helper_path, |command| {
        command.arg(PROVIDER_HOST_OPERATION);
    })? {
        HelperExecution::TimedOut(output) => bail!(
            "private decoder Provider Host inventory timed out after {} seconds{}",
            HELPER_TIMEOUT.as_secs(),
            helper_stderr_suffix(&output.stderr)
        ),
        HelperExecution::Completed(output) => decode_provider_host_inventory(&output),
    }
}

fn decode_provider_host_inventory(output: &HelperProcessOutput) -> Result<ProviderHostInventory> {
    if !output.status.success() {
        bail!(
            "private decoder Provider Host inventory exited with {}{}",
            output.status,
            helper_stderr_suffix(&output.stderr)
        );
    }
    decode_provider_host_inventory_stdout(&output.stdout)
}

fn decode_provider_host_inventory_stdout(stdout: &[u8]) -> Result<ProviderHostInventory> {
    let text = std::str::from_utf8(stdout)
        .map_err(|_| anyhow::anyhow!("private decoder Provider Host inventory is not UTF-8"))?;
    let fields = text.split_whitespace().collect::<Vec<_>>();
    if fields.len() != 4
        || fields[0] != PROVIDER_HOST_PROTOCOL
        || fields[1] != PROVIDER_HOST_OPERATION
    {
        bail!("private decoder Provider Host returned an invalid inventory envelope");
    }
    let private_provider_available = match fields[2] {
        "0" => false,
        "1" => true,
        _ => bail!("private decoder Provider Host returned an invalid availability flag"),
    };
    let router_version =
        decode_printable_identity_field(fields[3], "Provider Host router version")?;
    if private_provider_available && !router_version.contains(";private=") {
        bail!("private decoder Provider Host availability lacks a private graph identity");
    }
    if !private_provider_available && router_version.contains(";private=") {
        bail!("private decoder Provider Host public inventory contains a private graph identity");
    }
    Ok(ProviderHostInventory {
        private_provider_available,
        router_version,
    })
}

#[cfg(test)]
mod tests;
