use std::{
    fs,
    path::PathBuf,
    sync::atomic::{AtomicU64, Ordering},
};

use super::{
    RawFoundationNoiseAssessmentError, RawFoundationNoiseLevel, assess_staged_bayer_noise,
};

static FIXTURE_SEQUENCE: AtomicU64 = AtomicU64::new(1);

struct StagedFixture {
    root: PathBuf,
    manifest: PathBuf,
}

impl Drop for StagedFixture {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.root);
    }
}

#[test]
fn smooth_low_moderate_and_high_noise_are_distinguished() {
    let low = staged_noise_fixture(0.001_7);
    let moderate = staged_noise_fixture(0.004_2);
    let high = staged_noise_fixture(0.008_0);

    let low_assessment = assess_staged_bayer_noise(&low.manifest).expect("low assessment");
    let moderate_assessment =
        assess_staged_bayer_noise(&moderate.manifest).expect("moderate assessment");
    let high_assessment = assess_staged_bayer_noise(&high.manifest).expect("high assessment");

    assert_eq!(low_assessment.level, RawFoundationNoiseLevel::Low);
    assert_eq!(moderate_assessment.level, RawFoundationNoiseLevel::Moderate);
    assert_eq!(high_assessment.level, RawFoundationNoiseLevel::High);
    assert!(low_assessment.score_percent < moderate_assessment.score_percent);
    assert!(moderate_assessment.score_percent < high_assessment.score_percent);
    assert!(low_assessment.confidence_percent >= 50);
}

#[test]
fn unsupported_staging_fails_closed() {
    let fixture = staged_noise_fixture(0.002);
    fs::write(
        &fixture.manifest,
        "shadow-raw-frame-staging-20260806.1 width=256 height=256 cfa=RGBW black=64,64,64,64 white=16383,16383,16383,16383 sample_bytes=131072\n",
    )
    .expect("replace manifest");
    assert!(matches!(
        assess_staged_bayer_noise(&fixture.manifest),
        Err(RawFoundationNoiseAssessmentError::InvalidManifest(_))
    ));
}

#[allow(
    clippy::cast_possible_truncation,
    clippy::cast_precision_loss,
    clippy::cast_sign_loss
)] // Fixture coordinates are <= 256 and the rounded sample is clamped to the u16 sensor range.
fn staged_noise_fixture(noise_sigma: f64) -> StagedFixture {
    const WIDTH: usize = 256;
    const HEIGHT: usize = 256;
    const BLACK: f64 = 64.0;
    const WHITE: f64 = 16_383.0;
    let sequence = FIXTURE_SEQUENCE.fetch_add(1, Ordering::Relaxed);
    let root = std::env::temp_dir().join(format!(
        "shadow-noise-assessment-{}-{sequence}",
        std::process::id()
    ));
    fs::create_dir(&root).expect("fixture directory");
    let manifest = root.join("frame.shadowrawi");
    let samples_path = PathBuf::from(format!("{}.u16le", manifest.display()));
    let mut encoded = Vec::with_capacity(WIDTH * HEIGHT * 2);
    let mut random = 0x91e1_0da5_c79e_7b1d_u64;
    for y in 0..HEIGHT {
        for x in 0..WIDTH {
            // A planar source is deliberately retained: the estimator should
            // cancel scene gradients rather than mistake them for noise.
            let signal = 0.15 + 0.08 * x as f64 / WIDTH as f64 + 0.04 * y as f64 / HEIGHT as f64;
            let noisy = (signal + approximate_gaussian(&mut random) * noise_sigma).clamp(0.0, 1.0);
            let sample = (BLACK + noisy * (WHITE - BLACK)).round() as u16;
            encoded.extend_from_slice(&sample.to_le_bytes());
        }
    }
    fs::write(&samples_path, encoded).expect("sample plane");
    fs::write(
        &manifest,
        format!(
            "shadow-raw-frame-staging-20260806.1 width={WIDTH} height={HEIGHT} cfa=RGGB black=64,64,64,64 white=16383,16383,16383,16383 provider_id_hex=- provider_version_hex=- sample_bytes={}\n",
            WIDTH * HEIGHT * 2
        ),
    )
    .expect("manifest");
    StagedFixture { root, manifest }
}

fn approximate_gaussian(state: &mut u64) -> f64 {
    // Sum-of-uniforms has unit variance after subtracting six. It is stable,
    // deterministic, and needs no random test dependency.
    (0..12).map(|_| uniform(state)).sum::<f64>() - 6.0
}

#[allow(clippy::cast_precision_loss)] // Every sampled integer is bounded to 53 exactly representable bits.
fn uniform(state: &mut u64) -> f64 {
    *state ^= *state << 13;
    *state ^= *state >> 7;
    *state ^= *state << 17;
    (*state >> 11) as f64 / ((1_u64 << 53) - 1) as f64
}
