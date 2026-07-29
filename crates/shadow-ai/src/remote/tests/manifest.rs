use crate::{
    ProviderExecutionClass,
    remote::{
        REMOTE_PROVIDER_MANIFEST_SCHEMA_VERSION, RemoteProviderManifest,
        RemoteProviderManifestError,
    },
};

use super::provider_fixture::manifest;

#[test]
fn remote_manifest_round_trips_without_raw_or_sensor_upload_scopes() {
    let manifest = manifest();
    manifest.validate().expect("valid remote manifest");

    let json = serde_json::to_string(&manifest).expect("serialize");
    assert!(!json.contains("raw_file"));
    assert!(!json.contains("sensor_mosaic"));
    assert!(!json.contains("scene_linear"));
    assert_eq!(
        serde_json::from_str::<RemoteProviderManifest>(&json).expect("deserialize"),
        manifest
    );
    assert_eq!(
        manifest.execution_route().provider.execution_class,
        ProviderExecutionClass::RemoteService
    );
}

#[test]
fn remote_manifest_requires_the_exact_schema() {
    let mut manifest = manifest();
    manifest.schema_version = REMOTE_PROVIDER_MANIFEST_SCHEMA_VERSION + 1;
    assert_eq!(
        manifest.validate(),
        Err(RemoteProviderManifestError::UnsupportedSchemaVersion(
            REMOTE_PROVIDER_MANIFEST_SCHEMA_VERSION + 1
        ))
    );
}

#[test]
fn remote_manifest_rejects_unknown_serialized_fields() {
    let mut value = serde_json::to_value(manifest()).expect("serialize");
    value.as_object_mut().expect("manifest object").insert(
        "endpoint_token".into(),
        serde_json::Value::String("secret".into()),
    );
    assert!(serde_json::from_value::<RemoteProviderManifest>(value).is_err());
}
