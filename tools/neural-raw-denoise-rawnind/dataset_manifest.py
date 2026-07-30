#!/usr/bin/env python3
"""Pin a small, official RawNIND known-sensor evaluation set.

This module never downloads RAW payloads. It consumes one saved Dataverse
metadata response and the pinned RawNIND dataset descriptor, verifies both
identities, and emits a deterministic download/evaluation manifest.
"""

from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
import hashlib
import json
from pathlib import Path
import re
from typing import Any

import public_pair


SCHEMA = "shadow-rawnind-evaluation-manifest-v1"
PILOT_MANIFEST_SHA256 = (
    "6e095434b2baa231b2c595b6c404ed96"
    "ce84c04655d7a1adf16cec0f6817b205"
)
DATASET_API_URL = (
    "https://dataverse.uclouvain.be/api/datasets/:persistentId/"
    "?persistentId=doi:10.14428/DVN/DEQCIM"
)
DATASET_DOI = public_pair.DATASET_DOI
DATASET_URL = public_pair.DATASET_URL
DATASET_ID = 10_364
DATASET_VERSION = "1.0"
DATASET_VERSION_STATE = "RELEASED"
DATASET_RELEASE_TIME = "2025-01-15T20:46:22Z"
DATASET_LICENSE = public_pair.DATASET_LICENSE
DATASET_FILE_COUNT = 2_845
DATASET_TOTAL_BYTES = 120_172_531_162


@dataclass(frozen=True)
class PinnedAuxiliaryFile:
    dataverse_file_id: int
    persistent_id: str
    filename: str
    size_bytes: int
    md5: str


DATASET_DESCRIPTOR = PinnedAuxiliaryFile(
    dataverse_file_id=30_027,
    persistent_id="doi:10.14428/DVN/DEQCIM/WWGHOR",
    filename="dataset.yaml",
    size_bytes=456_897,
    md5="9400717d01b524222ccc00c1fb584a14",
)


@dataclass(frozen=True)
class PilotSelection:
    scene: str
    camera_make: str
    camera_model: str
    noisy_iso: int
    ground_truth_filename: str
    noisy_filename: str


# These are the ten known-sensor scenes identified by the RawNIND paper's
# evaluation set. Every file must also belong to a descriptor scene marked
# `test_reserve: true`.
PILOT_SELECTIONS = (
    PilotSelection(
        "7D-2",
        "Canon",
        "EOS 7D",
        12_800,
        (
            "Bayer_TEST_7D-2_GT_ISO100_"
            "sha1=45803fc993c395c515b165a1f5d7417f4af4fe1e.cr2"
        ),
        (
            "Bayer_TEST_7D-2_ISO12800_"
            "sha1=f378ff0b6b1eea30b0184671a4a223dd93b4f271.cr2"
        ),
    ),
    PilotSelection(
        "MuseeL-Saint-Pierre-C500D",
        "Canon",
        "EOS 500D",
        3_200,
        (
            "Bayer_TEST_MuseeL-Saint-Pierre-C500D_GT_ISO100_"
            "sha1=f71bff2d267cefbf243971b98beb973daeee84db.cr2"
        ),
        (
            "Bayer_TEST_MuseeL-Saint-Pierre-C500D_ISO3200_"
            "sha1=4b71ad313b4fb8d70ba3c5d8cf5a5b9839f8ed80.cr2"
        ),
    ),
    PilotSelection(
        "Laura_Lemons_platformer",
        "Sony",
        "Alpha 7C",
        5_000,
        (
            "Bayer_TEST_Laura_Lemons_platformer_GT_ISO50_"
            "sha1=e65e60c925d304bbb01cf8bb1310c41e85a05d62.arw"
        ),
        (
            "Bayer_TEST_Laura_Lemons_platformer_ISO5000_"
            "sha1=e29fc8c8cf6505f84a3135ee82e6473884539c1a.arw"
        ),
    ),
    PilotSelection(
        "MuseeL-vases-A7C",
        "Sony",
        "Alpha 7C",
        12_800,
        (
            "Bayer_TEST_MuseeL-vases-A7C_GT_ISO50_"
            "sha1=3512b789004767165f4330b1e82c6fdce766b221.arw"
        ),
        (
            "Bayer_TEST_MuseeL-vases-A7C_ISO12800_"
            "sha1=7d67a13a47ade45e608a1630354a378a0d899e63.arw"
        ),
    ),
    PilotSelection(
        "TitusToys",
        "Sony",
        "Alpha 7C",
        25_600,
        (
            "Bayer_TEST_TitusToys_GT_ISO50_"
            "sha1=85c54ed5174f0ca97385984e6301a1fab08cbdbe.arw"
        ),
        (
            "Bayer_TEST_TitusToys_ISO25600_"
            "sha1=d7a77e6f2d966ca6cd3106df3511646456fa3490.arw"
        ),
    ),
    PilotSelection(
        "Vaxt-i-trad",
        "Canon",
        "EOS 6D",
        12_800,
        (
            "Bayer_TEST_Vaxt-i-trad_GT_ISO100_"
            "sha1=ca77ec885c0198357c413895c343b965c1ebcafb.cr2"
        ),
        (
            "Bayer_TEST_Vaxt-i-trad_ISO12800_"
            "sha1=d61bb9bdc1aaea030d66b5ed34e76c66255d340c.cr2"
        ),
    ),
    PilotSelection(
        "Pen-pile",
        "Panasonic",
        "Lumix DMC-GH1",
        3_200,
        (
            "Bayer_TEST_Pen-pile_GT_ISO100_"
            "sha1=dead5469f750d32cd50f8673f1ae3608bdb9a99d.dng"
        ),
        (
            "Bayer_TEST_Pen-pile_ISO3200_"
            "sha1=a3221270fde0f61f690b4b4619d74ec32dce6e31.dng"
        ),
    ),
    PilotSelection(
        "boardgames_top",
        "Sony",
        "Alpha 7C",
        10_000,
        (
            "Bayer_TEST_boardgames_top_GT_ISO50_"
            "sha1=8c121e3c1038766ee9f96565376eb264cf15b509.arw"
        ),
        (
            "Bayer_TEST_boardgames_top_ISO10000_"
            "sha1=c6e8e34be44a4b5d4b26b3a0339e646483225553.arw"
        ),
    ),
    PilotSelection(
        "MuseeL-bluebirds-A7C",
        "Sony",
        "Alpha 7C",
        16_000,
        (
            "Bayer_TEST_MuseeL-bluebirds-A7C_GT_ISO50_"
            "sha1=3156b462861d9e67f6e0305d8d55ad695a4d937c.arw"
        ),
        (
            "Bayer_TEST_MuseeL-bluebirds-A7C_ISO16000_"
            "sha1=e4031d1853423a4f294807864845d60da22eecdd.arw"
        ),
    ),
    PilotSelection(
        "D60-1",
        "Canon",
        "EOS D60",
        1_037,
        (
            "Bayer_TEST_D60-1_GT_ISO100_"
            "sha1=8ed4d50624a199ed767b2c7167b172a1d5eb4031.crw"
        ),
        (
            "Bayer_TEST_D60-1_ISO1037_"
            "sha1=b192a6f4c6c46c7f5d3c22f85d2f1a2dd1174b80.crw"
        ),
    ),
)


@dataclass(frozen=True)
class DescriptorScene:
    unknown_sensor: bool
    test_reserve: bool
    clean_images: tuple[str, ...]
    noisy_images: tuple[str, ...]


_SCENE_RE = re.compile(r"^  ([^\s:#][^:]*)\:$")
_BOOL_RE = re.compile(r"^    (unknown_sensor|test_reserve): (true|false)$")
_LIST_RE = re.compile(r"^    (clean_images|noisy_images):$")
_FILENAME_RE = re.compile(r"^    - filename: (.+)$")
_SHA1_SUFFIX_RE = re.compile(r"_sha1=([0-9a-f]{40})\.[^.]+$")
_MD5_RE = re.compile(r"^[0-9a-f]{32}$")


def _md5(path: Path) -> str:
    digest = hashlib.md5(usedforsecurity=False)
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def verify_auxiliary_file(
    path: Path,
    expected: PinnedAuxiliaryFile,
) -> None:
    resolved = path.resolve()
    if not resolved.is_file():
        raise ValueError(f"RawNIND {expected.filename} does not exist")
    if resolved.stat().st_size != expected.size_bytes:
        raise ValueError(f"RawNIND {expected.filename} size mismatch")
    if _md5(resolved) != expected.md5:
        raise ValueError(f"RawNIND {expected.filename} MD5 mismatch")


def parse_pinned_bayer_descriptor(path: Path) -> dict[str, DescriptorScene]:
    """Parse only the schema needed from the MD5-pinned descriptor."""

    verify_auxiliary_file(path, DATASET_DESCRIPTOR)
    scenes: dict[str, dict[str, Any]] = {}
    in_bayer = False
    current_scene: str | None = None
    current_list: str | None = None
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.rstrip()
        if line == "Bayer:":
            in_bayer = True
            current_scene = None
            current_list = None
            continue
        if in_bayer and line and not line.startswith(" "):
            break
        if not in_bayer or not line:
            continue
        scene_match = _SCENE_RE.fullmatch(line)
        if scene_match is not None:
            current_scene = scene_match.group(1)
            if current_scene in scenes:
                raise ValueError("RawNIND descriptor has a duplicate Bayer scene")
            scenes[current_scene] = {
                "unknown_sensor": None,
                "test_reserve": None,
                "clean_images": [],
                "noisy_images": [],
            }
            current_list = None
            continue
        if current_scene is None:
            continue
        bool_match = _BOOL_RE.fullmatch(line)
        if bool_match is not None:
            key, text = bool_match.groups()
            scenes[current_scene][key] = text == "true"
            continue
        list_match = _LIST_RE.fullmatch(line)
        if list_match is not None:
            current_list = list_match.group(1)
            continue
        filename_match = _FILENAME_RE.fullmatch(line)
        if filename_match is not None:
            if current_list is None:
                raise ValueError("RawNIND descriptor filename has no list owner")
            filename = filename_match.group(1)
            if _SHA1_SUFFIX_RE.search(filename) is None:
                raise ValueError("RawNIND descriptor filename lacks a SHA-1 suffix")
            scenes[current_scene][current_list].append(filename)

    parsed: dict[str, DescriptorScene] = {}
    for name, scene in scenes.items():
        if scene["unknown_sensor"] is None or scene["test_reserve"] is None:
            raise ValueError("RawNIND descriptor scene lacks split metadata")
        parsed[name] = DescriptorScene(
            unknown_sensor=scene["unknown_sensor"],
            test_reserve=scene["test_reserve"],
            clean_images=tuple(scene["clean_images"]),
            noisy_images=tuple(scene["noisy_images"]),
        )
    if not parsed:
        raise ValueError("RawNIND descriptor contains no Bayer scenes")
    return parsed


def _file_identity(data_file: dict[str, Any]) -> dict[str, Any]:
    return {
        "dataverse_file_id": data_file.get("id"),
        "persistent_id": data_file.get("persistentId"),
        "filename": data_file.get("filename"),
        "size_bytes": data_file.get("filesize"),
        "md5": data_file.get("md5"),
    }


def _validated_inventory(metadata_path: Path) -> dict[str, dict[str, Any]]:
    try:
        response = json.loads(metadata_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError("RawNIND Dataverse metadata is not valid JSON") from error
    if response.get("status") != "OK":
        raise ValueError("RawNIND Dataverse metadata status is not OK")
    data = response.get("data")
    if not isinstance(data, dict) or data.get("id") != DATASET_ID:
        raise ValueError("RawNIND Dataverse dataset identity mismatch")
    version = data.get("latestVersion")
    if not isinstance(version, dict):
        raise ValueError("RawNIND Dataverse latest version is missing")
    actual_version = (
        f"{version.get('versionNumber')}.{version.get('versionMinorNumber')}"
    )
    if actual_version != DATASET_VERSION:
        raise ValueError("RawNIND Dataverse version mismatch")
    if version.get("versionState") != DATASET_VERSION_STATE:
        raise ValueError("RawNIND Dataverse version is not released")
    if version.get("releaseTime") != DATASET_RELEASE_TIME:
        raise ValueError("RawNIND Dataverse release time mismatch")
    license_value = version.get("license")
    if (
        not isinstance(license_value, dict)
        or license_value.get("rightsIdentifier") != DATASET_LICENSE
    ):
        raise ValueError("RawNIND Dataverse license mismatch")
    files = version.get("files")
    if not isinstance(files, list) or len(files) != DATASET_FILE_COUNT:
        raise ValueError("RawNIND Dataverse file count mismatch")

    inventory: dict[str, dict[str, Any]] = {}
    total_bytes = 0
    for wrapper in files:
        if not isinstance(wrapper, dict) or wrapper.get("restricted") is not False:
            raise ValueError("RawNIND Dataverse contains an unavailable file")
        data_file = wrapper.get("dataFile")
        if not isinstance(data_file, dict):
            raise ValueError("RawNIND Dataverse file entry is malformed")
        filename = data_file.get("filename")
        size = data_file.get("filesize")
        md5 = data_file.get("md5")
        if not isinstance(filename, str) or filename in inventory:
            raise ValueError("RawNIND Dataverse filename is missing or duplicated")
        if not isinstance(size, int) or size <= 0:
            raise ValueError("RawNIND Dataverse file size is invalid")
        if not isinstance(md5, str) or _MD5_RE.fullmatch(md5) is None:
            raise ValueError("RawNIND Dataverse MD5 is invalid")
        inventory[filename] = data_file
        total_bytes += size
    if total_bytes != DATASET_TOTAL_BYTES:
        raise ValueError("RawNIND Dataverse total byte count mismatch")
    auxiliary = inventory.get(DATASET_DESCRIPTOR.filename)
    if auxiliary is None or _file_identity(auxiliary) != asdict(DATASET_DESCRIPTOR):
        raise ValueError("RawNIND dataset descriptor identity mismatch")
    return inventory


def _public_raw(
    inventory: dict[str, dict[str, Any]],
    filename: str,
    role: str,
) -> public_pair.PublicRawFile:
    data_file = inventory.get(filename)
    if data_file is None:
        raise ValueError(f"RawNIND selected file is absent: {filename}")
    return public_pair.PublicRawFile(role=role, **_file_identity(data_file))


def _canonical_json(value: object) -> bytes:
    return json.dumps(
        value,
        ensure_ascii=True,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")


def build_pilot_manifest(
    metadata_path: Path,
    descriptor_path: Path,
) -> dict[str, Any]:
    inventory = _validated_inventory(metadata_path)
    scenes = parse_pinned_bayer_descriptor(descriptor_path)
    pairs: list[dict[str, Any]] = []
    selected_filenames: set[str] = set()
    total_bytes = 0
    for selection in PILOT_SELECTIONS:
        scene = scenes.get(selection.scene)
        if scene is None:
            raise ValueError(f"RawNIND pilot scene is absent: {selection.scene}")
        if not scene.test_reserve or scene.unknown_sensor:
            raise ValueError(
                f"RawNIND pilot scene is not a known-sensor reserve: "
                f"{selection.scene}"
            )
        if selection.ground_truth_filename not in scene.clean_images:
            raise ValueError("RawNIND pilot ground truth is outside its scene")
        if selection.noisy_filename not in scene.noisy_images:
            raise ValueError("RawNIND pilot noisy RAW is outside its scene")
        ground_truth = _public_raw(
            inventory,
            selection.ground_truth_filename,
            "ground_truth",
        )
        noisy = _public_raw(inventory, selection.noisy_filename, "noisy")
        for value in (ground_truth, noisy):
            if value.filename in selected_filenames:
                raise ValueError("RawNIND pilot reuses a RAW payload")
            selected_filenames.add(value.filename)
            total_bytes += value.size_bytes
        pairs.append(
            {
                "scene": selection.scene,
                "camera_make": selection.camera_make,
                "camera_model": selection.camera_model,
                "noisy_iso": selection.noisy_iso,
                "ground_truth": asdict(ground_truth),
                "noisy": asdict(noisy),
            }
        )

    camera_makes = sorted({pair["camera_make"] for pair in pairs})
    cameras = sorted(
        {
            f"{pair['camera_make']} {pair['camera_model']}"
            for pair in pairs
        }
    )
    if len(pairs) != 10 or len(camera_makes) < 3 or len(cameras) < 6:
        raise ValueError("RawNIND pilot diversity contract is incomplete")
    body: dict[str, Any] = {
        "schema": SCHEMA,
        "dataset": {
            "doi": DATASET_DOI,
            "url": DATASET_URL,
            "api_url": DATASET_API_URL,
            "dataverse_dataset_id": DATASET_ID,
            "version": DATASET_VERSION,
            "version_state": DATASET_VERSION_STATE,
            "release_time": DATASET_RELEASE_TIME,
            "license": DATASET_LICENSE,
            "file_count": DATASET_FILE_COUNT,
            "total_bytes": DATASET_TOTAL_BYTES,
        },
        "selection_contract": {
            "split": "official-known-sensor-test-reserve",
            "source_descriptor": asdict(DATASET_DESCRIPTOR),
            "pair_count": len(pairs),
            "raw_file_count": len(selected_filenames),
            "camera_makes": camera_makes,
            "cameras": cameras,
            "policy": (
                "one pinned high-noise exposure and one pinned clean exposure "
                "per official known-sensor test scene"
            ),
        },
        "download": {
            "total_bytes": total_bytes,
            "files": [
                {
                    "filename": value["filename"],
                    "size_bytes": value["size_bytes"],
                    "url": (
                        "https://dataverse.uclouvain.be/api/access/datafile/"
                        f"{value['dataverse_file_id']}"
                    ),
                }
                for pair in pairs
                for value in (pair["ground_truth"], pair["noisy"])
            ],
        },
        "pairs": pairs,
    }
    identity = hashlib.sha256(_canonical_json(body)).hexdigest()
    if identity != PILOT_MANIFEST_SHA256:
        raise ValueError("RawNIND pilot manifest differs from the pinned identity")
    body["manifest_sha256"] = identity
    return body


def validate_manifest(value: dict[str, Any]) -> None:
    if value.get("schema") != SCHEMA:
        raise ValueError("RawNIND benchmark manifest schema mismatch")
    identity = value.get("manifest_sha256")
    if identity != PILOT_MANIFEST_SHA256:
        raise ValueError("RawNIND benchmark manifest identity is not pinned")
    body = dict(value)
    del body["manifest_sha256"]
    if hashlib.sha256(_canonical_json(body)).hexdigest() != identity:
        raise ValueError("RawNIND benchmark manifest identity mismatch")
    pairs = value.get("pairs")
    if not isinstance(pairs, list) or len(pairs) != len(PILOT_SELECTIONS):
        raise ValueError("RawNIND benchmark manifest pair count mismatch")


def load_manifest(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError("RawNIND benchmark manifest is not valid JSON") from error
    if not isinstance(value, dict):
        raise ValueError("RawNIND benchmark manifest root must be an object")
    validate_manifest(value)
    return value


def write_manifest(value: dict[str, Any], path: Path) -> None:
    validate_manifest(value)
    resolved = path.resolve()
    if resolved.exists():
        raise ValueError("RawNIND manifest output must not already exist")
    resolved.parent.mkdir(parents=True, exist_ok=True)
    with resolved.open("x", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2, sort_keys=True)
        stream.write("\n")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dataverse-metadata", type=Path, required=True)
    parser.add_argument("--dataset-descriptor", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    manifest = build_pilot_manifest(
        arguments.dataverse_metadata,
        arguments.dataset_descriptor,
    )
    write_manifest(manifest, arguments.output)
    print(json.dumps(manifest, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ValueError as error:
        print(f"error: {error}")
        raise SystemExit(2) from error
