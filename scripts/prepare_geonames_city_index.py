#!/usr/bin/env python3
"""Build Shadow's compact city-level index from official GeoNames exports.

The command performs no network access. Pass a directory containing
`cities500.zip`, `countryInfo.txt`, and `admin1CodesASCII.txt`, downloaded from
https://download.geonames.org/export/dump/.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import os
from pathlib import Path
import tempfile
import zipfile


MAGIC = "# shadow-geonames-city-index-v1"
LICENSE_ID = "CC-BY-4.0"


def clean_field(value: str) -> str:
    return " ".join(value.replace("\x00", " ").split())


def source_identity(base_version: str, paths: list[Path]) -> str:
    digest = hashlib.sha256()
    for path in paths:
        digest.update(path.name.encode("ascii"))
        digest.update(b"\x00")
        with path.open("rb") as source:
            while chunk := source.read(1024 * 1024):
                digest.update(chunk)
        digest.update(b"\x00")
    return f"{base_version}-sha256-{digest.hexdigest()}"


def load_countries(path: Path) -> dict[str, str]:
    countries: dict[str, str] = {}
    with path.open("r", encoding="utf-8", newline="") as source:
        for raw_line in source:
            if raw_line.startswith("#") or not raw_line.strip():
                continue
            fields = raw_line.rstrip("\r\n").split("\t")
            if len(fields) < 5:
                raise ValueError("countryInfo.txt contains a malformed row")
            code = fields[0].strip().upper()
            name = clean_field(fields[4])
            if len(code) != 2 or not code.isascii() or not code.isalpha() or not name:
                raise ValueError("countryInfo.txt contains an invalid country")
            countries[code] = name
    if not countries:
        raise ValueError("countryInfo.txt contains no countries")
    return countries


def load_admin1(path: Path) -> dict[str, str]:
    areas: dict[str, str] = {}
    with path.open("r", encoding="utf-8", newline="") as source:
        reader = csv.reader(source, delimiter="\t")
        for fields in reader:
            if len(fields) != 4:
                raise ValueError("admin1CodesASCII.txt contains a malformed row")
            code = fields[0].strip()
            name = clean_field(fields[1])
            if not code or not name:
                raise ValueError("admin1CodesASCII.txt contains an invalid area")
            areas[code] = name
    if not areas:
        raise ValueError("admin1CodesASCII.txt contains no administrative areas")
    return areas


def load_cities(
    archive_path: Path,
    countries: dict[str, str],
    areas: dict[str, str],
) -> list[tuple[int, int, int, str, str, str, str]]:
    rows: list[tuple[int, int, int, str, str, str, str]] = []
    with zipfile.ZipFile(archive_path, "r") as archive:
        members = [name for name in archive.namelist() if name.endswith(".txt")]
        if len(members) != 1:
            raise ValueError("cities500.zip must contain exactly one text export")
        with archive.open(members[0], "r") as binary_source:
            for binary_line in binary_source:
                fields = binary_line.decode("utf-8").rstrip("\r\n").split("\t")
                if len(fields) != 19:
                    raise ValueError("cities500.txt contains a malformed row")
                if fields[6] != "P":
                    continue
                code = fields[8].strip().upper()
                country = countries.get(code)
                locality = clean_field(fields[1])
                if country is None or not locality:
                    continue
                latitude_e7 = round(float(fields[4]) * 10_000_000)
                longitude_e7 = round(float(fields[5]) * 10_000_000)
                population = int(fields[14] or "0")
                if not -900_000_000 <= latitude_e7 <= 900_000_000:
                    raise ValueError("cities500.txt contains an invalid latitude")
                if not -1_800_000_000 <= longitude_e7 <= 1_800_000_000:
                    raise ValueError("cities500.txt contains an invalid longitude")
                if not 0 <= population <= 4_294_967_295:
                    raise ValueError("cities500.txt contains an invalid population")
                area = areas.get(f"{code}.{fields[10].strip()}", "")
                rows.append(
                    (
                        latitude_e7,
                        longitude_e7,
                        population,
                        code,
                        country,
                        clean_field(area),
                        locality,
                    )
                )
    if not rows:
        raise ValueError("cities500.zip contains no usable populated places")
    rows.sort(key=lambda row: (row[0], row[1], -row[2], row[6]))
    return rows


def write_index(
    output_path: Path,
    dataset_version: str,
    rows: list[tuple[int, int, int, str, str, str, str]],
) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{output_path.name}.", dir=output_path.parent
    )
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as output:
            output.write(f"{MAGIC}\t{dataset_version}\t{LICENSE_ID}\n")
            writer = csv.writer(output, delimiter="\t", lineterminator="\n")
            writer.writerows(rows)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary_name, output_path)
    except BaseException:
        try:
            os.unlink(temporary_name)
        except FileNotFoundError:
            pass
        raise


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build Shadow's offline GeoNames city index without network access."
    )
    parser.add_argument("--source-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--dataset-version",
        required=True,
        help="Immutable source snapshot identity, for example geonames-2026-08-02",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    if not args.dataset_version or len(args.dataset_version) > 40 or not all(
        character.isascii() and (character.isalnum() or character in "._-")
        for character in args.dataset_version
    ):
        raise ValueError("dataset version must use only ASCII letters, digits, '.', '_' or '-'")
    cities_path = args.source_dir / "cities500.zip"
    countries_path = args.source_dir / "countryInfo.txt"
    areas_path = args.source_dir / "admin1CodesASCII.txt"
    identity = source_identity(args.dataset_version, [cities_path, countries_path, areas_path])
    countries = load_countries(countries_path)
    areas = load_admin1(areas_path)
    rows = load_cities(cities_path, countries, areas)
    write_index(args.output, identity, rows)
    print(f"wrote {len(rows)} cities to {args.output} ({identity})")


if __name__ == "__main__":
    main()
