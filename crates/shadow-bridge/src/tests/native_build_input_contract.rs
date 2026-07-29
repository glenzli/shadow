use std::{fs, path::Path};

fn embedded_additional_inputs(build_script: &str) -> &str {
    build_script
        .split_once("const EMBEDDED_IMAGE_ADDITIONAL_INPUTS")
        .and_then(|(_, remainder)| remainder.split_once("];"))
        .map(|(inputs, _)| inputs)
        .expect("shadow-bridge build script must retain its explicit native input manifest")
}

#[test]
fn direct_cargo_build_tracks_mask_coverage_header_owners() {
    let crate_root = Path::new(env!("CARGO_MANIFEST_DIR"));
    let image_root = crate_root.join("../../cpp/shadow-image");
    let build_script = fs::read_to_string(crate_root.join("build.rs"))
        .expect("shadow-bridge build script must be readable");
    let inputs = embedded_additional_inputs(&build_script);

    for relative_path in [
        "src/edit/photo_geometry_sampling.hpp",
        "src/proxy/warm_edit_gpu_mask_msl.hpp",
    ] {
        assert!(
            image_root.join(relative_path).is_file(),
            "tracked native input does not exist: {relative_path}"
        );
        assert!(
            inputs.contains(&format!("\"{relative_path}\"")),
            "direct Cargo builds must rerun when {relative_path} changes"
        );
    }
}
