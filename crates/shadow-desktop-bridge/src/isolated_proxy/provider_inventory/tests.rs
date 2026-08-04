use std::fmt::Write as _;

use super::decode_provider_host_inventory_stdout;

fn hex_text(value: &str) -> String {
    value.bytes().fold(String::new(), |mut output, byte| {
        write!(output, "{byte:02x}").expect("write hex into String");
        output
    })
}

#[test]
fn accepts_verified_private_provider_inventory() {
    let version = "router=1;raw=abc;private=def;private_module=123";
    let receipt = format!(
        "shadow-provider-host-v1 provider-inventory 1 {}\n",
        hex_text(version)
    );
    let inventory =
        decode_provider_host_inventory_stdout(receipt.as_bytes()).expect("decode inventory");
    assert!(inventory.private_provider_available);
    assert_eq!(inventory.router_version, version);
}

#[test]
fn accepts_public_only_provider_host_inventory() {
    let version = "router=1;raw=abc;raster=def";
    let receipt = format!(
        "shadow-provider-host-v1 provider-inventory 0 {}\n",
        hex_text(version)
    );
    let inventory =
        decode_provider_host_inventory_stdout(receipt.as_bytes()).expect("decode inventory");
    assert!(!inventory.private_provider_available);
    assert_eq!(inventory.router_version, version);
}

#[test]
fn rejects_availability_without_private_graph_identity() {
    let receipt = format!(
        "shadow-provider-host-v1 provider-inventory 1 {}\n",
        hex_text("router=1;raw=abc")
    );
    let error = decode_provider_host_inventory_stdout(receipt.as_bytes())
        .expect_err("reject inconsistent availability");
    assert!(error.to_string().contains("lacks a private graph identity"));
}
