//! Transient people-list and facial-region composition for node-bound masks.
//!
//! Person numbers and thumbnails exist only for one AI-mask input session.
//! They are not identity recognition, biometric storage, or reusable mask
//! assets. One parsed label map can be recomposed locally for many region
//! combinations without another provider request.

use std::{collections::BTreeSet, io::Cursor, sync::Arc};

use image::{DynamicImage, ImageFormat, codecs::jpeg::JpegEncoder, imageops::FilterType};
use shadow_ai::{DetectedFace, FaceBoundingBox, RasterExtent, VisionProvenance};
use thiserror::Error;

pub(crate) const MAX_SUBJECT_MASK_PEOPLE: usize = 16;
pub(crate) const FACE_REGION_COUNT: u32 = 11;
const PERSON_THUMBNAIL_EDGE: u32 = 96;
const PERSON_THUMBNAIL_JPEG_QUALITY: u8 = 86;
const MAX_PERSON_THUMBNAIL_BYTES: usize = 128 * 1024;
const MAX_ALL_PERSON_THUMBNAIL_BYTES: usize = 2 * 1024 * 1024;

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
#[repr(u8)]
pub(crate) enum FaceRegion {
    Face = 0,
    Skin = 1,
    Eyes = 2,
    Eyebrows = 3,
    LipsAndMouth = 4,
    Nose = 5,
    Ears = 6,
    Hair = 7,
    Neck = 8,
    Clothing = 9,
    Accessories = 10,
}

impl FaceRegion {
    const ALL: [Self; FACE_REGION_COUNT as usize] = [
        Self::Face,
        Self::Skin,
        Self::Eyes,
        Self::Eyebrows,
        Self::LipsAndMouth,
        Self::Nose,
        Self::Ears,
        Self::Hair,
        Self::Neck,
        Self::Clothing,
        Self::Accessories,
    ];

    const fn bit(self) -> u16 {
        1_u16 << self as u8
    }

    const fn classes(self) -> &'static [u8] {
        match self {
            Self::Face => &[1, 2, 3, 4, 5, 7, 8, 10, 11, 12, 13],
            Self::Skin => &[1],
            Self::Eyes => &[4, 5],
            Self::Eyebrows => &[2, 3],
            Self::LipsAndMouth => &[11, 12, 13],
            Self::Nose => &[10],
            Self::Ears => &[7, 8],
            Self::Hair => &[17],
            Self::Neck => &[14],
            Self::Clothing => &[16],
            Self::Accessories => &[6, 9, 15, 18],
        }
    }
}

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub(crate) struct FaceRegionSet(u16);

impl FaceRegionSet {
    pub(crate) const KNOWN_BITS: u16 = (1_u16 << FACE_REGION_COUNT) - 1;

    pub(crate) fn from_bits(bits: u32) -> Result<Self, SubjectMaskPeopleError> {
        let bits = u16::try_from(bits).map_err(|_| SubjectMaskPeopleError::InvalidRegionSet)?;
        if bits == 0 || bits & !Self::KNOWN_BITS != 0 {
            return Err(SubjectMaskPeopleError::InvalidRegionSet);
        }
        Ok(Self(bits))
    }

    pub(crate) const fn bits(self) -> u32 {
        self.0 as u32
    }

    pub(crate) const fn contains(self, region: FaceRegion) -> bool {
        self.0 & region.bit() != 0
    }

    pub(crate) const fn intersects(self, other: Self) -> bool {
        self.0 & other.0 != 0
    }
}

#[derive(Debug, Clone)]
pub(crate) struct SubjectMaskPersonCandidate {
    pub(crate) bounding_box: FaceBoundingBox,
    pub(crate) confidence: f32,
    pub(crate) thumbnail_jpeg: Arc<[u8]>,
}

impl SubjectMaskPersonCandidate {
    pub(crate) fn resident_byte_len(&self) -> usize {
        self.thumbnail_jpeg.len()
    }
}

#[derive(Debug, Clone)]
pub(crate) struct ParsedSubjectMaskPerson {
    pub(crate) labels: Arc<[u8]>,
    pub(crate) extent: RasterExtent,
    pub(crate) provenance: VisionProvenance,
    pub(crate) available_regions: FaceRegionSet,
}

impl ParsedSubjectMaskPerson {
    pub(crate) fn new(
        labels: Vec<u8>,
        extent: RasterExtent,
        provenance: VisionProvenance,
    ) -> Result<Self, SubjectMaskPeopleError> {
        validate_label_count(&labels, extent)?;
        let available_regions = available_face_regions(&labels);
        Ok(Self {
            labels: labels.into(),
            extent,
            provenance,
            available_regions,
        })
    }

    pub(crate) fn resident_byte_len(&self) -> usize {
        self.labels.len()
    }
}

pub(crate) fn prepare_person_candidates(
    input_jpeg: &[u8],
    input_extent: RasterExtent,
    detections: &[DetectedFace],
) -> Result<Vec<SubjectMaskPersonCandidate>, SubjectMaskPeopleError> {
    let decoded = image::load_from_memory_with_format(input_jpeg, ImageFormat::Jpeg)
        .map_err(SubjectMaskPeopleError::DecodeInput)?;
    if decoded.width() != input_extent.width || decoded.height() != input_extent.height {
        return Err(SubjectMaskPeopleError::InputGeometryMismatch);
    }

    let mut ordered = detections.to_vec();
    ordered.sort_by(|left, right| {
        face_center(left.bounding_box)
            .0
            .total_cmp(&face_center(right.bounding_box).0)
            .then_with(|| {
                face_center(left.bounding_box)
                    .1
                    .total_cmp(&face_center(right.bounding_box).1)
            })
            .then_with(|| right.confidence.total_cmp(&left.confidence))
    });
    ordered.truncate(MAX_SUBJECT_MASK_PEOPLE);

    let mut total_thumbnail_bytes = 0_usize;
    ordered
        .into_iter()
        .map(|detection| {
            let thumbnail = face_thumbnail(&decoded, detection.bounding_box)?;
            total_thumbnail_bytes = total_thumbnail_bytes
                .checked_add(thumbnail.len())
                .ok_or(SubjectMaskPeopleError::ThumbnailBudgetExceeded)?;
            if total_thumbnail_bytes > MAX_ALL_PERSON_THUMBNAIL_BYTES {
                return Err(SubjectMaskPeopleError::ThumbnailBudgetExceeded);
            }
            Ok(SubjectMaskPersonCandidate {
                bounding_box: detection.bounding_box,
                confidence: detection.confidence,
                thumbnail_jpeg: thumbnail.into(),
            })
        })
        .collect()
}

pub(crate) fn compose_face_region_mask(
    parsed: &ParsedSubjectMaskPerson,
    regions: FaceRegionSet,
) -> Result<Vec<u8>, SubjectMaskPeopleError> {
    validate_label_count(&parsed.labels, parsed.extent)?;
    let selected_classes = FaceRegion::ALL
        .into_iter()
        .filter(|region| regions.contains(*region))
        .flat_map(FaceRegion::classes)
        .copied()
        .collect::<BTreeSet<_>>();
    let samples = parsed
        .labels
        .iter()
        .map(|label| {
            if selected_classes.contains(label) {
                u8::MAX
            } else {
                0
            }
        })
        .collect::<Vec<_>>();
    if samples.iter().all(|sample| *sample == 0) {
        return Err(SubjectMaskPeopleError::SelectedRegionsUnavailable);
    }
    Ok(samples)
}

fn available_face_regions(labels: &[u8]) -> FaceRegionSet {
    let mut bits = 0_u16;
    for region in FaceRegion::ALL {
        if labels.iter().any(|label| region.classes().contains(label)) {
            bits |= region.bit();
        }
    }
    FaceRegionSet(bits)
}

fn face_thumbnail(
    image: &DynamicImage,
    bounds: FaceBoundingBox,
) -> Result<Vec<u8>, SubjectMaskPeopleError> {
    let image_width = image.width() as f32;
    let image_height = image.height() as f32;
    let center = face_center(bounds);
    let side = bounds.width.max(bounds.height).mul_add(1.45, 0.0).max(1.0);
    let left = (center.0 - side * 0.5)
        .floor()
        .clamp(0.0, image_width - 1.0);
    let top = (center.1 - side * 0.5)
        .floor()
        .clamp(0.0, image_height - 1.0);
    let right = (center.0 + side * 0.5)
        .ceil()
        .clamp(left + 1.0, image_width);
    let bottom = (center.1 + side * 0.5)
        .ceil()
        .clamp(top + 1.0, image_height);
    let crop = image.crop_imm(
        left as u32,
        top as u32,
        (right - left) as u32,
        (bottom - top) as u32,
    );
    let thumbnail = crop.resize_to_fill(
        PERSON_THUMBNAIL_EDGE,
        PERSON_THUMBNAIL_EDGE,
        FilterType::Triangle,
    );
    let mut encoded = Vec::new();
    JpegEncoder::new_with_quality(Cursor::new(&mut encoded), PERSON_THUMBNAIL_JPEG_QUALITY)
        .encode_image(&thumbnail)
        .map_err(SubjectMaskPeopleError::EncodeThumbnail)?;
    if encoded.is_empty() || encoded.len() > MAX_PERSON_THUMBNAIL_BYTES {
        return Err(SubjectMaskPeopleError::ThumbnailBudgetExceeded);
    }
    Ok(encoded)
}

fn face_center(bounds: FaceBoundingBox) -> (f32, f32) {
    (
        bounds.x + bounds.width * 0.5,
        bounds.y + bounds.height * 0.5,
    )
}

fn validate_label_count(labels: &[u8], extent: RasterExtent) -> Result<(), SubjectMaskPeopleError> {
    let expected = u64::from(extent.width)
        .checked_mul(u64::from(extent.height))
        .and_then(|value| usize::try_from(value).ok())
        .ok_or(SubjectMaskPeopleError::InvalidLabelMap)?;
    if labels.len() != expected {
        Err(SubjectMaskPeopleError::InvalidLabelMap)
    } else {
        Ok(())
    }
}

#[derive(Debug, Error)]
pub(crate) enum SubjectMaskPeopleError {
    #[error("the people-mask region set is empty or contains unknown regions")]
    InvalidRegionSet,
    #[error("the people-mask input JPEG could not be decoded")]
    DecodeInput(#[source] image::ImageError),
    #[error("the people-mask input geometry changed after face detection")]
    InputGeometryMismatch,
    #[error("the people thumbnail could not be encoded")]
    EncodeThumbnail(#[source] image::ImageError),
    #[error("people thumbnails exceed the bounded transient byte budget")]
    ThumbnailBudgetExceeded,
    #[error("the face-parsing label map is malformed")]
    InvalidLabelMap,
    #[error("the selected facial regions are not visible for this person")]
    SelectedRegionsUnavailable,
}

#[cfg(test)]
mod tests;
