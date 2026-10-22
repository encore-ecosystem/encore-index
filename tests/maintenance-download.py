#!/usr/bin/env python3
"""Public archive redirects are distinct from authenticated registry traffic."""
import importlib.util
from pathlib import Path
import urllib.request
import unittest

spec = importlib.util.spec_from_file_location("maintenance", Path(__file__).resolve().parents[1] / "maintenance.py")
maintenance = importlib.util.module_from_spec(spec)
spec.loader.exec_module(maintenance)


class RedirectTests(unittest.TestCase):
    def redirect(self, handler, destination, headers=None):
        request = urllib.request.Request("https://github.com/owner/repo/releases/download/v1/pkg.tar.gz", headers=headers or {})
        return handler.redirect_request(request, None, 302, "Found", {}, destination)

    def test_public_https_asset(self):
        request = self.redirect(maintenance.ArchiveRedirect(), "https://release-assets.githubusercontent.com/archive")
        self.assertFalse(request.has_header("Authorization"))

    def test_no_downgrade_or_file_redirect(self):
        for url in ("http://example.org/archive", "file:///etc/passwd", "https://user:secret@example.org/archive"):
            with self.assertRaises(ValueError):
                self.redirect(maintenance.ArchiveRedirect(), url)

    def test_credentials_cannot_redirect(self):
        for header in ("Authorization", "Cookie"):
            with self.assertRaises(ValueError):
                self.redirect(maintenance.ArchiveRedirect(), "https://example.org/archive", {header: "disposable"})
        with self.assertRaises(ValueError):
            self.redirect(maintenance.NoRedirect(), "https://example.org/archive")


if __name__ == "__main__":
    unittest.main()
