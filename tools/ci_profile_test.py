"""Tests of ci_profile.py, the CI profile summary (step 26.16).

The committed fixture (ci_profile_testdata/fast_profile.json) is a trimmed
profile of a real `bazel test --config=fast` run, scrubbed of paths outside
the repository. Its @dcfs_llvm events come from a `bazel fetch --force
--repo=@dcfs_llvm` profile of the same checkout, and the fast run's events are
shifted 1926 s later, as if one invocation fetched LLVM and then built. The
expected table (expected.md) was added up from the fixture's events by a
separate sweep-line script, not by ci_profile.py. The other tests use small
inline profiles whose numbers are easy to add up.
"""

import gzip
import io
import json
import os
import sys
import tempfile
import unittest

import ci_profile

ARGS = {}  # name -> path, from the command line in main()


def ev(cat, name, ts, dur, **extra):
    e = {"cat": cat, "name": name, "ph": "X", "ts": ts, "dur": dur, "pid": 1, "tid": 1}
    e.update(extra)
    return e


def action(name, ts, dur, target=None, mnemonic="CppCompile", out=None):
    args = {"mnemonic": mnemonic}
    if target is not None:
        args["target"] = target
    extra = {"args": args}
    if out is not None:
        extra["out"] = out
    return ev("action processing", name, ts, dur, **extra)


def fetch(repo, ts, dur):
    return ev("Fetching repository", "@@" + repo, ts, dur)


def run(paths):
    out = io.StringIO()
    status = ci_profile.main(paths, out)
    return status, out.getvalue()


def write(directory, name, events):
    path = os.path.join(directory, name)
    doc = {"otherData": {}, "traceEvents": events}
    opener = gzip.open if name.endswith(".gz") else open
    with opener(path, "wt", encoding="utf-8") as f:
        json.dump(doc, f)
    return path


class Fixture(unittest.TestCase):
    def test_table_of_a_real_profile(self):
        status, got = run([ARGS["fixture"]])
        with open(ARGS["expected"], encoding="utf-8") as f:
            want = f.read()
        self.assertEqual(status, 0)
        self.assertEqual(got, want)


class Classify(unittest.TestCase):
    def key(self, **kw):
        return ci_profile.classify(action("a", 0, 1, **kw))

    def test_tests(self):
        self.assertEqual(self.key(target="//dcfs:x_test", mnemonic="TestRunner"), "test")
        # A test of a third-party package is still a test.
        self.assertEqual(self.key(target="//third_party/alpine:busybox_test", mnemonic="TestRunner"), "test")

    def test_third_party(self):
        self.assertEqual(self.key(target="@@gawk+//:gawk"), "third_party")
        self.assertEqual(self.key(target="//third_party/debian:rootfs"), "third_party")
        # Not under third_party/: a prefix of the name is not the package.
        self.assertEqual(self.key(target="//third_party_not:x"), "other")

    def test_ours(self):
        for label in ("//dcfs:main", "//tools:testutil", "//bench:b", "//test/qemu:t", "//dcfs/testonly:f"):
            self.assertEqual(self.key(target=label), "ours", label)

    def test_other(self):
        self.assertEqual(self.key(target="//formal:model"), "other")
        self.assertEqual(self.key(target="//:MODULE"), "other")

    def test_no_target_falls_back_to_the_output(self):
        a = ci_profile.classify(action("a", 0, 1, out="bazel-out/k8-opt-exec/bin/external/gawk+/_objs/main.o"))
        self.assertEqual(a, "third_party")
        a = ci_profile.classify(action("a", 0, 1, out="bazel-out/k8-fastbuild/bin/dcfs/main.o"))
        self.assertEqual(a, "other")
        self.assertEqual(ci_profile.classify(action("a", 0, 1)), "other")

    def test_llvm_is_found_by_the_repository_name(self):
        self.assertEqual(ci_profile.classify(fetch("+llvm_distribution+dcfs_llvm", 0, 1)), "llvm")
        self.assertEqual(ci_profile.classify(fetch("dcfs_llvm", 0, 1)), "llvm")
        self.assertEqual(ci_profile.classify(fetch("toolchains_llvm+", 0, 1)), "fetch")
        self.assertEqual(ci_profile.classify(fetch("+x+dcfs_llvm_extra", 0, 1)), "fetch")

    def test_other_events_count_nowhere(self):
        self.assertIsNone(ci_profile.classify(ev("general information", "x", 0, 1)))


class Analyze(unittest.TestCase):
    def test_union_of_intervals(self):
        self.assertEqual(ci_profile.union([]), 0)
        self.assertEqual(ci_profile.union([(0, 10), (5, 15), (20, 25)]), 20)
        self.assertEqual(ci_profile.union([(0, 100), (10, 20)]), 100)
        self.assertEqual(ci_profile.union([(30, 40), (0, 10)]), 20)

    def test_rows(self):
        events = [
            ev("build phase marker", "Launch Blaze", -1_000_000, 1_000_000),
            ev("build phase marker", "Complete build", 100_000_000, 0, ph="i"),  # an instant
            fetch("a+", 0, 10_000_000),
            fetch("b+", 5_000_000, 10_000_000),  # overlaps a+: wall 15, busy 20
            fetch("+llvm_distribution+dcfs_llvm", 0, 40_000_000),
            action("c", 50_000_000, 2_000_000, "@@gawk+//:gawk"),
            action("d", 50_000_000, 3_000_000, "//dcfs:main"),
            action("e", 52_000_000, 1_000_000, "//tools:t"),
            action("t", 60_000_000, 30_000_000, "//dcfs:x_test", "TestRunner"),
            ev("critical path component", "action 'Compiling x'", 50_000_000, 3_000_000),
            ev("critical path component", "action 'Testing y'", 60_000_000, 30_000_000),
        ]
        r = ci_profile.analyze(events)
        self.assertEqual(r["total"], 101_000_000)
        rows = r["rows"]
        self.assertEqual(rows["fetch"], (15_000_000, 20_000_000, 2))
        self.assertEqual(rows["llvm"], (40_000_000, 40_000_000, 1))
        self.assertEqual(rows["third_party"], (2_000_000, 2_000_000, 1))
        # d and e overlap by 1 s (50-53 and 52-53).
        self.assertEqual(rows["ours"], (3_000_000, 4_000_000, 2))
        self.assertEqual(rows["other"], (0, 0, 0))
        self.assertEqual(rows["test"], (30_000_000, 30_000_000, 1))
        self.assertEqual(rows["critical"][1:], (33_000_000, 2))
        self.assertEqual(r["longest"][0], (30_000_000, "action 'Testing y'"))

    def test_without_phase_markers_the_span_of_the_events_is_the_total(self):
        r = ci_profile.analyze([ev("x", "a", 5_000_000, 1_000_000), ev("x", "b", 9_000_000, 2_000_000)])
        self.assertEqual(r["total"], 6_000_000)
        self.assertEqual(ci_profile.analyze([])["total"], 0)

    def test_render(self):
        r = ci_profile.analyze([fetch("a+", 0, 1_500_000)])
        text = ci_profile.render("job", r)
        self.assertIn("#### job\n", text)
        self.assertIn("| Repository fetches (not @dcfs_llvm) | 1.5 | 1.5 | 1 |\n", text)
        self.assertIn("| Total wall | 1.5 | | |\n", text)
        self.assertNotIn("Longest critical path", text)


class Main(unittest.TestCase):
    def test_files_gzipped_or_not_and_several(self):
        with tempfile.TemporaryDirectory() as d:
            a = write(d, "full-0.json.gz", [fetch("a+", 0, 2_000_000)])
            b = write(d, "other.json", [fetch("b+", 0, 3_000_000)])
            status, got = run([a, b])
        self.assertEqual(status, 0)
        self.assertEqual(got.count("#### "), 2)
        self.assertIn("#### full-0\n", got)
        self.assertIn("#### other\n", got)
        self.assertIn("| Repository fetches (not @dcfs_llvm) | 2.0 |", got)
        self.assertIn("| Repository fetches (not @dcfs_llvm) | 3.0 |", got)

    def test_an_unreadable_profile_is_status_2_and_the_others_still_print(self):
        with tempfile.TemporaryDirectory() as d:
            good = write(d, "good.json", [fetch("a+", 0, 1_000_000)])
            bad = os.path.join(d, "bad.json")
            with open(bad, "w", encoding="utf-8") as f:
                f.write("{truncated")
            status, got = run([bad, good, os.path.join(d, "missing.json")])
        self.assertEqual(status, 2)
        self.assertEqual(got.count("#### "), 1)
        self.assertIn("#### good\n", got)

    def test_no_arguments_is_a_usage_error(self):
        self.assertEqual(run([])[0], 2)


if __name__ == "__main__":
    for arg in sys.argv[1:]:
        if "=" in arg and not arg.startswith("-"):
            k, v = arg.split("=", 1)
            ARGS[k] = v
    sys.argv = sys.argv[:1]
    unittest.main()
