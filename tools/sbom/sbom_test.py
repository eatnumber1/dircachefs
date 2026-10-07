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


def alpine_docs():
    """{repository name: resolved.json text} of the fetched Alpine repos."""
    return {k: read(k) for k in REPO if k.startswith("alpine_")}


def build(module=None, lock=None, graph=None, debs=None, sources=None,
          prepare=None, pins=None, alpine=None):
    """Returns sbom.build's {"shipped": doc, "testonly": doc}."""
    return sbom.build(
        read("module") if module is None else module,
        read("lock") if lock is None else lock,
        read("graph") if graph is None else graph,
        read("debs") if debs is None else debs,
        read("sources") if sources is None else sources,
        read("prepare") if prepare is None else prepare,
        json.loads(read("pins")) if pins is None else pins,
        alpine=alpine_docs() if alpine is None else alpine)


def components(docs=None):
    docs = docs or build()
    return docs["shipped"]["components"] + docs["testonly"]["components"]


class RealPins(unittest.TestCase):
    def test_every_pin_has_an_entry(self):
        covered = sbom.pins_covered(build())
        module = read("module")
        # Independent of sbom.parse_module: a regexp over the text.
        deps = set(re.findall(r'^bazel_dep\(\s*name\s*=\s*"([^"]+)"', module, re.M))
        repos = set(re.findall(
            r'^(?:http_archive|http_file)\(\s*name\s*=\s*"([^"]+)"',
            module, re.M))
        self.assertGreaterEqual(len(deps), 10)
        self.assertGreaterEqual(len(repos), 5)
        for d in deps:
            self.assertIn(f"bazel_dep:{d}", covered)
        for r in repos:
            self.assertIn(f"repository:{r}", covered)
        self.assertIn("script:bazelisk", covered)
        for line in read("debs").splitlines():
            if line.strip():
                binary = line.split("_")[0]
                self.assertIn(f"deb:{binary}", covered)
        # Every package an alpine_package names is in the SBOM (by its name
        # in the pin, its purl uses the origin: AlpineComponents).
        packages = re.findall(
            r'^alpine_package\(.*?packages\s*=\s*\[([^\]]*)\]', module,
            re.M | re.S)
        self.assertTrue(packages)
        for group in packages:
            for name in re.findall(r'"([^"]+)"', group):
                self.assertIn(f"alpine:{name}", covered)

    def test_pins_json_has_no_entry_for_a_pin_that_is_gone(self):
        pins = json.loads(read("pins"))
        deps, repos = sbom.parse_module(read("module"))
        for name in pins["repository"]:
            self.assertIn(name, repos,
                          f"pins.json: repository {name} is not in MODULE.bazel")
        for name in pins["bazel_dep"]:
            self.assertIn(name, deps,
                          f"pins.json: bazel_dep {name} is not in MODULE.bazel")

    def test_debian_purls_are_osv_shaped(self):
        comps = build()["testonly"]["components"]
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
        purls = {c["purl"] for c in components()}
        self.assertIn("pkg:github/nektos/act@0.2.89", purls)
        self.assertIn("pkg:github/tlaplus/tlaplus@1.7.4", purls)

    def test_every_entry_says_whether_osv_can_match_it(self):
        for c in build()["testonly"]["components"]:
            props = {p["name"]: p["value"] for p in c["properties"] if p["name"] != "dcfs:pin"}
            self.assertIn(props["dcfs:osv-matchable"], ("true", "false"))
            self.assertEqual(props["dcfs:osv-matchable"] == "true",
                             c["purl"].startswith(("pkg:deb/", "pkg:apk/")))


class AlpineComponents(unittest.TestCase):
    """The packages of the Alpine repositories (third_party/alpine)."""

    def resolved(self):
        return {name: json.loads(text) for name, text in alpine_docs().items()}

    def test_components_are_the_fetched_packages_under_their_origin(self):
        purls = {c["purl"] for c in build()["testonly"]["components"]}
        self.assertTrue(self.resolved())
        for doc in self.resolved().values():
            distro = "alpine-" + doc["branch"].lstrip("v")
            self.assertRegex(distro, r"^alpine-[0-9]+\.[0-9]+$")
            for p in doc["packages"]:
                self.assertIn(
                    f"pkg:apk/alpine/{p['origin']}@{p['version']}"
                    f"?distro={distro}", purls)

    def test_no_component_is_a_binary_subpackage_name(self):
        # OSV matches Alpine advisories by origin package: a component named
        # after a binary subpackage (linux-virt, whose origin is linux-lts)
        # finds nothing, silently.
        purls = {c["purl"] for c in build()["testonly"]["components"]}
        subpackages = {
            p["name"] for doc in self.resolved().values()
            for p in doc["packages"] if p["name"] != p["origin"]}
        self.assertIn("linux-virt", subpackages)
        origins = {p["origin"] for doc in self.resolved().values()
                   for p in doc["packages"]}
        self.assertNotIn("linux-virt", origins)
        for name in subpackages - origins:
            self.assertFalse(
                any(p.startswith(f"pkg:apk/alpine/{name}@") for p in purls),
                name)

    def test_the_kernel_is_reported_as_linux_lts(self):
        purls = {c["purl"] for c in build()["testonly"]["components"]}
        self.assertTrue(any(p.startswith("pkg:apk/alpine/linux-lts@6.")
                            for p in purls), sorted(purls)[:5])

    def test_the_repositories_come_from_module_bazel(self):
        self.assertEqual(sorted(sbom.parse_alpine_repos(read("module"))),
                         sorted(alpine_docs()))

    def test_an_alpine_repository_without_resolved_json_fails(self):
        with self.assertRaisesRegex(sbom.SbomError, "alpine_linux_virt"):
            build(alpine={k: v for k, v in alpine_docs().items()
                          if k != "alpine_linux_virt"})

    def test_a_resolved_json_nothing_declares_fails(self):
        docs = dict(alpine_docs(), alpine_stray=read("alpine_linux_virt"))
        with self.assertRaisesRegex(sbom.SbomError, "alpine_stray"):
            build(alpine=docs)

    def test_a_resolved_json_without_the_named_package_fails(self):
        doc = json.loads(read("alpine_linux_virt"))
        doc["packages"] = []
        with self.assertRaisesRegex(sbom.SbomError, "linux-virt"):
            build(alpine=dict(alpine_docs(),
                              alpine_linux_virt=json.dumps(doc)))


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
        del pins["repository"]["pjdfstest"]
        with self.assertRaisesRegex(sbom.SbomError, "pjdfstest"):
            build(pins=pins)


class Shipped(unittest.TestCase):
    """The shipped SBOM is what the Bazel graph of the binaries links."""

    def pins(self):
        return json.loads(read("pins"))

    def test_graph_lists_the_linked_repositories(self):
        # Guards the graph file itself: a genquery that went empty would make
        # every check below pass vacuously.
        repos = sbom.graph_repos(read("graph"))
        for r in ("abseil-cpp+", "sqlite3+", "libfuse+", "liburing+", "numactl+"):
            self.assertIn(r, repos)

    def test_every_external_repo_in_the_graph_is_accounted_for(self):
        pins = self.pins()
        shipped = {k + "+" for k in pins["shipped"]}
        build_only = set(pins["build_only"])
        for r in sbom.graph_repos(read("graph")):
            self.assertIn(r, shipped | build_only,
                          f"{r} is linked into the binaries but has no entry")

    def test_shipped_sbom_has_every_shipped_repo_and_nothing_else(self):
        docs = build()
        names = {c["name"] for c in docs["shipped"]["components"]}
        expected = {r.rstrip("+") for r in sbom.graph_repos(read("graph"))
                    if r not in self.pins()["build_only"]}
        self.assertEqual({self.pins()["shipped"][m].get("name", m)
                          for m in expected} | set(self.pins()["toolchain_runtime"]),
                         names)

    def test_the_toolchains_static_runtime_is_shipped(self):
        # libc++, libc++abi, libunwind and compiler-rt's builtins are in
        # every binary: llvm-project at the llvm_version of MODULE.bazel.
        by_name = {c["name"]: c for c in build()["shipped"]["components"]}
        llvm = by_name["llvm-project"]
        self.assertEqual(llvm["version"], "22.1.8")
        self.assertIn("ca7933e47d3a3451d81e72ac174dcb5aa28b59d1", llvm["purl"])

    def test_llvm_version_change_without_a_new_pin_fails(self):
        module = read("module").replace('llvm_version = "22.1.8"',
                                        'llvm_version = "22.1.9"')
        with self.assertRaisesRegex(sbom.SbomError, "llvm-project.*22.1.8"):
            build(module=module)

    def test_no_test_only_repo_is_in_the_shipped_sbom(self):
        docs = build()
        shipped_pins = sbom.pins_covered(docs["shipped"])
        testonly_pins = sbom.pins_covered(docs["testonly"])
        self.assertEqual(shipped_pins & testonly_pins, set())
        # The things only tests use must not be linked.
        graph = sbom.graph_repos(read("graph"))
        for r in ("googletest+", "google_benchmark+", "pjdfstest", "tla2tools",
                  "act", "rules_python+", "+alpine_package+alpine_linux_virt",
                  "+alpine_package+alpine_qemu",
                  "+alpine_package+alpine_fstools",
                  "+alpine_package+alpine_busybox",
                  "+alpine_package+alpine_dmsetup",
                  "+alpine_package+alpine_strace"):
            self.assertNotIn(r, graph)
        for c in docs["shipped"]["components"]:
            self.assertNotIn("deb:", "".join(
                p["value"] for p in c["properties"] if p["name"] == "dcfs:pin"))
            self.assertIn(("dcfs:scope", "shipped"),
                          {(p["name"], p["value"]) for p in c["properties"]})

    def test_without_a_graph_every_shipped_pin_is_used(self):
        docs = sbom.build(read("module"), read("lock"), None, read("debs"),
                          read("sources"), read("prepare"), self.pins(),
                          alpine=alpine_docs())
        self.assertEqual(docs["shipped"], build()["shipped"])

    def test_a_new_linked_repo_without_an_entry_fails(self):
        graph = read("graph") + "@@brand_new_lib+//lib:thing\n"
        with self.assertRaisesRegex(sbom.SbomError, "brand_new_lib"):
            build(graph=graph)

    def test_a_shipped_entry_nothing_links_fails(self):
        graph = "\n".join(l for l in read("graph").splitlines()
                          if not l.startswith("@@numactl+"))
        with self.assertRaisesRegex(sbom.SbomError, "numactl"):
            build(graph=graph)

    def test_shipped_entries_have_a_commit_and_a_github_purl(self):
        for c in build()["shipped"]["components"]:
            props = {p["name"]: p["value"] for p in c["properties"]}
            self.assertRegex(props["dcfs:commit"], r"^[0-9a-f]{40}$")
            self.assertRegex(c["purl"], r"^pkg:github/[^/@]+/[^/@]+@" + props["dcfs:commit"] + "$")
            self.assertTrue(props["dcfs:tag"])
            self.assertEqual(c["externalReferences"][0]["url"].split("/")[2], "github.com")

    def test_pins_version_must_equal_the_resolved_module_version(self):
        # The lock file records only the selected version's source.json.
        lock = read("lock").replace("/libfuse/3.18.2/source.json",
                                    "/libfuse/3.18.3/source.json")
        with self.assertRaisesRegex(sbom.SbomError, "libfuse.*3.18.2.*3.18.3"):
            build(lock=lock)

    def test_module_missing_from_the_lock_fails(self):
        lock = json.loads(read("lock"))
        lock["registryFileHashes"] = {
            k: v for k, v in lock["registryFileHashes"].items()
            if "/liburing/2.14/source.json" not in k}
        with self.assertRaisesRegex(sbom.SbomError, "liburing"):
            build(lock=json.dumps(lock))

    def test_a_bad_commit_fails(self):
        pins = self.pins()
        pins["shipped"]["libfuse"]["commit"] = "fuse-3.18.2"
        with self.assertRaisesRegex(sbom.SbomError, "libfuse.*commit"):
            build(pins=pins)

    def test_a_module_in_both_scopes_fails(self):
        pins = self.pins()
        pins["bazel_dep"]["libfuse"] = {"purl": "pkg:github/libfuse/libfuse", "kind": "code"}
        with self.assertRaisesRegex(sbom.SbomError, "libfuse"):
            build(pins=pins)

    def test_shipped_pins_record_osv_coverage(self):
        for m, p in self.pins()["shipped"].items():
            self.assertIn(p["osv"], ("records", "no-records"), m)


class GitRoots(unittest.TestCase):
    """osv-scanner matches commits only through a git root (README.md)."""

    def test_writes_one_detached_git_root_per_component(self):
        import tempfile
        doc = build()["shipped"]
        with tempfile.TemporaryDirectory() as out:
            roots = sbom.write_git_roots(doc, out)
            self.assertEqual(len(roots), len(doc["components"]))
            for c in doc["components"]:
                commit = [p["value"] for p in c["properties"]
                          if p["name"] == "dcfs:commit"][0]
                git = os.path.join(out, c["name"], ".git")
                self.assertEqual(open(os.path.join(git, "HEAD")).read(), commit + "\n")
                self.assertTrue(os.path.isdir(os.path.join(git, "objects")))
                self.assertTrue(os.path.isdir(os.path.join(git, "refs")))
                self.assertIn(c["externalReferences"][0]["url"],
                              open(os.path.join(git, "config")).read())

    def test_a_component_without_a_commit_fails(self):
        import tempfile
        doc = {"components": [{"name": "x", "properties": []}]}
        with tempfile.TemporaryDirectory() as out:
            with self.assertRaisesRegex(sbom.SbomError, "x"):
                sbom.write_git_roots(doc, out)


class Fixture(unittest.TestCase):
    def test_seeded_fixture_is_a_deliberately_old_libfuse(self):
        # libfuse 3.2.0 predates the fix of CVE-2018-10906, which OSV holds
        # as a GIT range: the scanner self-check in ci.yml must find it.
        doc = json.loads(read("fixture"))
        self.assertEqual([(c["name"], c["purl"]) for c in doc["components"]],
                         [("libfuse", "pkg:github/libfuse/libfuse@cfdca8c6a0f901f409d0a66dd158bd6c8b470bb6")])
        props = {p["name"]: p["value"] for p in doc["components"][0]["properties"]}
        self.assertEqual(props["dcfs:commit"], "cfdca8c6a0f901f409d0a66dd158bd6c8b470bb6")


class VerifyCommits(unittest.TestCase):
    def fake(self, refs):
        return lambda repo, patterns: refs

    def pins(self):
        return json.loads(read("pins"))["shipped"]

    def test_agreeing_commits_pass(self):
        pins = self.pins()
        def ls(repo, patterns):
            for p in pins.values():
                if p["repo"] == repo:
                    return f"{p['commit']}\trefs/tags/{p['tag']}\n"
        self.assertEqual(sbom.verify_commits(pins, ls), [])

    def test_annotated_tags_are_peeled(self):
        pins = self.pins()
        def ls(repo, patterns):
            for p in pins.values():
                if p["repo"] == repo:
                    return (f"{'0' * 40}\trefs/tags/{p['tag']}\n"
                            f"{p['commit']}\trefs/tags/{p['tag']}^{{}}\n")
        self.assertEqual(sbom.verify_commits(pins, ls), [])

    def test_a_moved_tag_is_reported(self):
        pins = self.pins()
        def ls(repo, patterns):
            for p in pins.values():
                if p["repo"] == repo:
                    return f"{'1' * 40}\trefs/tags/{p['tag']}\n"
        problems = sbom.verify_commits(pins, ls)
        self.assertEqual(len(problems), len(pins))
        self.assertIn("moved", problems[0])

    def test_a_missing_tag_is_reported(self):
        problems = sbom.verify_commits(self.pins(), lambda r, p: "")
        self.assertEqual(len(problems), len(self.pins()))
        self.assertIn("not found", problems[0])


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
