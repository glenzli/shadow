from __future__ import annotations

from pathlib import Path
import tempfile
import unittest

import public_pair


class RawNindPublicPairContract(unittest.TestCase):
    def test_download_urls_are_derived_from_pinned_dataverse_ids(self) -> None:
        self.assertEqual(
            public_pair.NOISY.download_url,
            "https://dataverse.uclouvain.be/api/access/datafile/28663",
        )
        self.assertEqual(
            public_pair.GROUND_TRUTH.download_url,
            "https://dataverse.uclouvain.be/api/access/datafile/28815",
        )

    def test_wrong_payload_is_rejected_before_raw_decode(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / public_pair.NOISY.filename
            path.write_bytes(b"not an official RawNIND RAW")
            with self.assertRaisesRegex(ValueError, "size mismatch"):
                public_pair.verify_public_raw(path, public_pair.NOISY)

    def test_assembly_rejects_unverified_parts_and_removes_temporary(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            parts = [root / "part0", root / "part1"]
            parts[0].write_bytes(b"not ")
            parts[1].write_bytes(b"official")
            output = root / public_pair.NOISY.filename
            with self.assertRaisesRegex(ValueError, "size mismatch"):
                public_pair.assemble_download_parts(
                    parts,
                    output,
                    public_pair.NOISY,
                )
            self.assertFalse(output.exists())
            self.assertFalse(output.with_name(output.name + ".partial").exists())


if __name__ == "__main__":
    unittest.main()
