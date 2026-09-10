#!/usr/bin/env python3
"""Focused archive-safety tests for validate-index.py."""

from __future__ import annotations

import hashlib
import importlib.util
import io
import tarfile
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

SCRIPT = Path(__file__).with_name("validate-index.py")
SPEC = importlib.util.spec_from_file_location("validate_index", SCRIPT)
assert SPEC and SPEC.loader
VALIDATOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VALIDATOR)


def archive(extra_name: str | None = None) -> bytes:
    output = io.BytesIO()
    manifest = (
        b'[project]\nname = "sample"\nversion = "1.2.3"\n'
        b'repository = "https://github.com/example/sample"\n'
        b'encore = ">=0.1.5,<0.2.0"\n'
    )
    with tarfile.open(fileobj=output, mode="w:gz") as package:
        entry = tarfile.TarInfo("encore.toml")
        entry.size = len(manifest)
        package.addfile(entry, io.BytesIO(manifest))
        if extra_name is not None:
            entry = tarfile.TarInfo(extra_name)
            entry.size = 1
            package.addfile(entry, io.BytesIO(b"x"))
    return output.getvalue()


def metadata(payload: bytes) -> dict:
    return {
        "version": "1.2.3",
        "encore": ">=0.1.5,<0.2.0",
        "archive": (
            "https://github.com/example/sample/releases/download/"
            "sample-v1.2.3/sample-1.2.3.tar.gz"
        ),
        "checksum": hashlib.sha256(payload).hexdigest(),
    }


class ArchiveValidationTests(unittest.TestCase):
    def test_download_without_base_checks_every_archive(self) -> None:
        first = {**metadata(archive()), "yanked": False}
        second = {**first, "version": "1.2.4", "yanked": True}
        document = {
            "name": "sample",
            "repository": "https://github.com/example/sample",
            "versions": [first, second],
        }
        with patch.object(VALIDATOR, "load", return_value=document):
            with patch.object(VALIDATOR, "verify_archive") as verify:
                VALIDATOR.validate(Path("sa/sample.json"), None, True, None, "legacy", False)
                self.assertEqual(verify.call_count, 2)
                self.assertEqual([call.args[2] for call in verify.call_args_list], [first, second])

    def test_download_without_base_propagates_archive_failure(self) -> None:
        document = {
            "name": "sample",
            "repository": "https://github.com/example/sample",
            "versions": [{**metadata(archive()), "yanked": False}],
        }
        with patch.object(VALIDATOR, "load", return_value=document):
            with patch.object(VALIDATOR, "verify_archive", side_effect=ValueError("bad archive")):
                with self.assertRaisesRegex(ValueError, "bad archive"):
                    VALIDATOR.validate(Path("sa/sample.json"), None, True, None, "legacy", False)

    def test_epoch_reset_preserves_repository_ownership(self) -> None:
        version = metadata(archive())
        version.update({
            "yanked": False, "epoch": "neumann", "nametag": "neumann",
            "release": "1.2.3-neumann",
        })
        old = {"name": "sample", "repository": "https://github.com/example/sample",
               "versions": []}
        current = {**old, "repository": "https://github.com/other/sample",
                   "versions": [version]}
        with patch.object(VALIDATOR, "load", return_value=current):
            with patch.object(VALIDATOR, "base_document", return_value=old):
                with self.assertRaisesRegex(ValueError, "repository ownership is immutable"):
                    VALIDATOR.validate(Path("sa/sample.json"), "HEAD", False, None, "neumann", True)

    def test_valid_archive_is_extracted(self) -> None:
        payload = archive("src/lib.enq")
        version = metadata(payload)
        version["bootstrap"] = True
        with tempfile.TemporaryDirectory() as directory:
            with patch.object(VALIDATOR.urllib.request, "urlopen", return_value=io.BytesIO(payload)):
                VALIDATOR.verify_archive(
                    "sample",
                    "https://github.com/example/sample",
                    version,
                    Path(directory),
                )
            self.assertTrue(Path(directory, "sample-1.2.3", "encore.toml").is_file())
            self.assertTrue(
                Path(directory, "sample-1.2.3", ".encore-index-bootstrap").is_file()
            )

    def test_named_release_is_accepted_during_epoch_reset(self) -> None:
        payload = archive()
        version = metadata(payload)
        version.update({
            "archive": (
                "https://github.com/example/sample/releases/download/"
                "sample-v1.2.3-neumann/sample-1.2.3.tar.gz"
            ),
            "commit": "a" * 40,
            "subdir": "",
            "yanked": False,
            "epoch": "neumann",
            "nametag": "neumann",
            "release": "1.2.3-neumann",
        })
        document = {
            "name": "sample",
            "repository": "https://github.com/example/sample",
            "versions": [version],
        }
        with patch.object(VALIDATOR, "load", return_value=document):
            with patch.object(VALIDATOR, "base_document", return_value=None):
                with patch.object(VALIDATOR, "verify_archive") as verify:
                    VALIDATOR.validate(Path("sa/sample.json"), "HEAD", True, None, "neumann", True)
                    verify.assert_called_once()

    def test_named_release_tag_must_match_metadata(self) -> None:
        payload = archive()
        version = metadata(payload)
        version.update({
            "commit": "a" * 40,
            "subdir": "",
            "yanked": False,
            "epoch": "neumann",
            "nametag": "neumann",
            "release": "1.2.3-neumann",
        })
        document = {
            "name": "sample",
            "repository": "https://github.com/example/sample",
            "versions": [version],
        }
        with patch.object(VALIDATOR, "load", return_value=document):
            with patch.object(VALIDATOR, "base_document", return_value=None):
                with self.assertRaisesRegex(ValueError, "archive does not belong"):
                    VALIDATOR.validate(Path("sa/sample.json"), "HEAD", False, None, "neumann", True)

    def test_parent_path_is_rejected(self) -> None:
        payload = archive("../escape")
        with patch.object(VALIDATOR.urllib.request, "urlopen", return_value=io.BytesIO(payload)):
            with self.assertRaisesRegex(ValueError, "unsafe archive member"):
                VALIDATOR.verify_archive(
                    "sample",
                    "https://github.com/example/sample",
                    metadata(payload),
                    None,
                )

    def test_manifest_identity_must_match(self) -> None:
        payload = archive()
        version = metadata(payload)
        version["version"] = "9.9.9"
        with patch.object(VALIDATOR.urllib.request, "urlopen", return_value=io.BytesIO(payload)):
            with self.assertRaisesRegex(ValueError, "manifest version"):
                VALIDATOR.verify_archive(
                    "sample",
                    "https://github.com/example/sample",
                    version,
                    None,
                )


if __name__ == "__main__":
    unittest.main()
