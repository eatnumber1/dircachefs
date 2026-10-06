"""Tests for sbom.py: every pin has an SBOM entry, and the ignore checks."""

import datetime
import json
import os
import re
import sys
import unittest

import sbom

REPO = {}  # name -> path, filled from the command line in main()


def read(name):
    with open(REPO[name], encoding="utf-8") as f:
        return f.read()


def build(module=None, debs=None, sources=None, prepare=None, pins=None):
    return sbom.build(
        read("module") if module is None else module,
        read("debs") if debs is None else debs,
        read("sources") if sources is None else sources,
        read("prepare") if prepare is None else prepare,
        json.loads(read("pins")) if pins is None else pins)


class RealPins(unittest.TestCase):
    def test_every_pin_has_an_entry(self):
        covered = sbom.pins_covered(build())
        module = read("module")
        # Independent of sbom.parse_module: a regexp over the text.
        deps = set(re.findall(r'^bazel_dep\(\s*name\s*=\s*"([^"]+)"', module, re.M))
        repos = set(re.findall(
            r'^(?:http_archive|http_file|qemu_repo)\(\s*name\s*=\s*"([^"]+)"',
            module, re.M))
        self.assertGreaterEqual(len(deps), 10)
        self.assertGreaterEqual(len(repos), 14)
        for d in deps:
            self.assertIn(f"bazel_dep:{d}", covered)
        for r in repos:
            self.assertIn(f"repository:{r}", covered)
        self.assertIn("repository:qemu.dtc", covered)
        self.assertIn("script:bazelisk", covered)
        for line in read("debs").splitlines():
            if line.strip():
                binary = line.split("_")[0]
                self.assertIn(f"deb:{binary}", covered)

    def test_debian_purls_are_osv_shaped(self):
        comps = build()["components"]
        debs = [c for c in comps if c["purl"].startswith("pkg:deb/debian/")]
        self.assertGreater(len(debs), 90)
        for c in debs:
            self.assertTrue(c["purl"].endswith("?distro=bookworm"), c["purl"])
            self.assertNotIn(":", c["purl"].split("@", 1)[1].split("?")[0])
        # A binary package is matched under its source package's name.
        purls = {c["purl"] for c in comps}
        self.assertTrue(any(p.startswith("pkg:deb/debian/glibc@") for p in purls))
        self.assertFalse(any(p.startswith("pkg:deb/debian/libc6@") for p in purls))

    def test_versions_come_from_the_pins(self):
        purls = {c["purl"] for c in build()["components"]}
        self.assertIn("pkg:generic/linux@7.2.9", purls)
        self.assertIn("pkg:generic/qemu@11.1.2", purls)
        self.assertIn("pkg:github/nektos/act@0.2.89", purls)
        self.assertIn("pkg:github/tlaplus/tlaplus@1.7.4", purls)
        self.assertIn("pkg:github/benhoyt/inih@r62", purls)

    def test_every_entry_says_whether_osv_can_match_it(self):
        for c in build()["components"]:
            props = {p["name"]: p["value"] for p in c["properties"] if p["name"] != "dcfs:pin"}
            self.assertIn(props["dcfs:osv-matchable"], ("true", "false"))
            self.assertEqual(props["dcfs:osv-matchable"] == "true",
                             c["purl"].startswith("pkg:deb/"))


class MissingEntries(unittest.TestCase):
    def test_new_http_archive_without_entry_fails(self):
        module = read("module") + '\nhttp_archive(name = "brand_new", url = "https://x/brand_new-1.0.tar.gz", strip_prefix = "brand_new-1.0", sha256 = "00")\n'
        with self.assertRaisesRegex(sbom.SbomError, "brand_new"):
            build(module=module)

    def test_new_http_file_without_entry_fails(self):
        module = read("module") + '\nhttp_file(name = "some_jar", urls = ["https://x/download/v1.0/a.jar"], sha256 = "00")\n'
        with self.assertRaisesRegex(sbom.SbomError, "some_jar"):
            build(module=module)

    def test_new_bazel_dep_without_entry_fails(self):
        module = read("module") + '\nbazel_dep(name = "new_dep", version = "1.0")\n'
        with self.assertRaisesRegex(sbom.SbomError, "new_dep"):
            build(module=module)

    def test_deb_without_a_source_row_fails(self):
        debs = read("debs") + "newpkg_1.0-1_amd64.deb " + "0" * 64 + "\n"
        with self.assertRaisesRegex(sbom.SbomError, "newpkg_1.0-1_amd64.deb"):
            build(debs=debs)

    def test_deb_version_change_without_a_new_row_fails(self):
        first = read("debs").splitlines()[0].split()
        name = first[0].split("_")[0]
        debs = f"{name}_99.0-1_amd64.deb {first[1]}\n"
        with self.assertRaises(sbom.SbomError):
            build(debs=debs)

    def test_pins_json_entry_removed_fails(self):
        pins = json.loads(read("pins"))
        del pins["repository"]["linux_source"]
        with self.assertRaisesRegex(sbom.SbomError, "linux_source"):
            build(pins=pins)


class Fixture(unittest.TestCase):
    def test_seeded_fixture_is_a_deliberately_old_zlib(self):
        doc = json.loads(read("fixture"))
        self.assertEqual([c["purl"] for c in doc["components"]],
                         ["pkg:deb/debian/zlib@1.2.11?distro=bookworm"])


TODAY = datetime.date(2026, 10, 6)


class Ignores(unittest.TestCase):
    def check(self, text):
        return sbom.check_ignores(text, TODAY)

    def test_valid(self):
        self.assertEqual(self.check(
            '[[IgnoredVulns]]\nid = "CVE-1"\nignoreUntil = 2026-12-01\nreason = "no fix"\n'), [])

    def test_empty_config_is_fine(self):
        self.assertEqual(self.check("# nothing ignored\n"), [])

    def test_expired(self):
        p = self.check('[[IgnoredVulns]]\nid = "CVE-1"\nignoreUntil = 2026-10-05\nreason = "r"\n')
        self.assertEqual(len(p), 1)
        self.assertIn("expired on 2026-10-05", p[0])

    def test_expires_today_is_still_valid(self):
        self.assertEqual(self.check('[[IgnoredVulns]]\nid = "CVE-1"\nignoreUntil = 2026-10-06\nreason = "r"\n'), [])

    def test_missing_reason_and_date(self):
        p = self.check('[[IgnoredVulns]]\nid = "CVE-1"\n')
        self.assertEqual(len(p), 2)

    def test_blank_reason(self):
        p = self.check('[[IgnoredVulns]]\nid = "CVE-1"\nignoreUntil = 2027-01-01\nreason = "  "\n')
        self.assertEqual(len(p), 1)

    def test_datetime_expiry(self):
        p = self.check('[[IgnoredVulns]]\nid = "CVE-1"\nignoreUntil = 2026-10-05T00:00:00Z\nreason = "r"\n')
        self.assertIn("expired", p[0])

    def test_not_a_date(self):
        p = self.check('[[IgnoredVulns]]\nid = "CVE-1"\nignoreUntil = "soon"\nreason = "r"\n')
        self.assertIn("not a date", p[0])

    def test_committed_config_is_clean(self):
        self.assertEqual(sbom.check_ignores(read("config"), datetime.datetime.now(
            datetime.timezone.utc).date()), [])


if __name__ == "__main__":
    for arg in sys.argv[1:]:
        if "=" in arg and not arg.startswith("-"):
            k, v = arg.split("=", 1)
            REPO[k] = v
    sys.argv = sys.argv[:1]
    unittest.main()
