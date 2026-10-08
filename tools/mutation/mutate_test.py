"""Tests of the mutation tester's operators over small synthetic AST nodes."""

import io
import json
import os
import stat
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout

import mutate

SOURCE = "if (!ok) { Mark(1); x = Kind::kFound; } y = a ? b : c;"


def expr(kind, start, text, **kw):
    first = mutate.TOKEN.match(text).group(0)
    last = mutate.TOKEN.findall(text)[-1]
    node = {"kind": kind, "_main": True,
            "range": {"begin": {"offset": start, "tokLen": len(first)},
                      "end": {"offset": start + len(text) - len(last), "tokLen": len(last)}}}
    node.update(kw)
    return node


class OperatorsTest(unittest.TestCase):

    def function(self, *stmts):
        return {"kind": "FunctionDecl", "name": "f", "_main": True,
                "inner": [{"kind": "CompoundStmt", "_main": True, "inner": list(stmts)}]}

    def test_negates_an_if_condition(self):
        cond = expr("UnaryOperator", SOURCE.index("!ok"), "!ok")
        found = mutate.mutants_in(self.function(
            {"kind": "IfStmt", "_main": True, "inner": [cond]}), SOURCE)
        self.assertEqual([(m[0], m[2], m[3]) for m in found],
                         [("negate-if", "!(!ok)", "!ok")])

    def test_deletes_a_status_phase_call_and_a_void_one(self):
        def call(qt, name, start, text):
            c = expr("CallExpr", start, text, type={"qualType": qt})
            c["inner"] = [{"kind": "DeclRefExpr", "referencedDecl": {"name": name}}]
            return c
        found = mutate.mutants_in(self.function(
            call("absl::Status", "MarkThing", SOURCE.index("Mark(1)"), "Mark(1)"),
            call("void", "EndThing", SOURCE.index("Mark(1)"), "Mark(1)"),
            call("absl::Status", "Other", SOURCE.index("Mark(1)"), "Mark(1)"),
            call("absl::StatusOr<int>", "BeginThing", SOURCE.index("Mark(1)"), "Mark(1)")), SOURCE)
        self.assertEqual([m[2] for m in found], ["absl::OkStatus()", "(void)0"])

    def test_swaps_present_and_absent(self):
        ref = expr("DeclRefExpr", SOURCE.index("Kind::kFound"), "Kind::kFound",
                   referencedDecl={"kind": "EnumConstantDecl", "name": "kFound"})
        found = mutate.mutants_in(self.function(ref), SOURCE)
        self.assertEqual([m[2] for m in found], ["Kind::kNegative"])

    def test_skips_a_span_that_does_not_start_at_a_token(self):
        self.assertFalse(mutate.sane((2, 5, 1), "a  b c"))
        self.assertTrue(mutate.sane((0, 1, 1), "a  b c"))

    def test_scope_rules(self):
        fn = self.function()
        self.assertTrue(mutate.selected(fn, [("all", [])]))
        self.assertTrue(mutate.selected(fn, [("names", ["f", "g"])]))
        self.assertFalse(mutate.selected(fn, [("names", ["g"])]))
        self.assertFalse(mutate.selected(fn, [("calls", ["^Mark"])]))


DIFF = """diff --git a/dcfs/x.cc b/dcfs/x.cc
--- a/dcfs/x.cc
+++ b/dcfs/x.cc
@@ -10,2 +10,3 @@ void F() {
@@ -40 +41 @@
@@ -60,3 +60,0 @@
"""


class ChangedTest(unittest.TestCase):
    """The per-push mode's mapping from git hunks to functions (26.5b)."""

    def test_hunks_are_new_file_line_ranges(self):
        # A pure deletion sits between its line and the next.
        self.assertEqual(mutate.parse_hunks(DIFF), [(10, 12), (41, 41), (60, 61)])

    def test_a_function_is_touched_when_a_hunk_overlaps_its_lines(self):
        hunks = mutate.parse_hunks(DIFF)
        self.assertTrue(mutate.touches((1, 10), hunks))    # the first line of a hunk
        self.assertTrue(mutate.touches((12, 30), hunks))   # the last
        self.assertTrue(mutate.touches((41, 41), hunks))
        self.assertTrue(mutate.touches((55, 60), hunks))   # a deletion after line 60
        self.assertFalse(mutate.touches((13, 40), hunks))  # between hunks
        self.assertFalse(mutate.touches((62, 90), hunks))
        self.assertFalse(mutate.touches((1, 9), hunks))

    def test_function_lines_come_from_the_node_range(self):
        source = "a\nbb\nccc\n"
        fn = {"range": {"begin": {"offset": 2, "tokLen": 2},
                        "end": {"offset": 5, "tokLen": 3}}}
        self.assertEqual(mutate.function_lines(fn, source), (2, 3))

    def test_the_range_of_a_new_branch_is_its_tip_against_the_parent(self):
        tip = "abc123"
        self.assertEqual(mutate.normalize_range("%s..%s" % ("0" * 40, tip)), "abc123~1..abc123")
        self.assertEqual(mutate.normalize_range("..abc123"), "abc123~1..abc123")
        self.assertEqual(mutate.normalize_range("a..b"), "a..b")

    def test_a_range_with_no_hunks_selects_nothing(self):
        self.assertEqual(mutate.parse_hunks(""), [])
        self.assertFalse(mutate.touches((1, 100), []))


class PrerequisitesTest(unittest.TestCase):
    """The AST dump needs the generated headers (libfuse_config.h) built first."""

    def test_the_build_asks_for_the_compile_prerequisites_of_the_target(self):
        self.assertEqual(mutate.prerequisites_args("//dcfs:dcfs_lib"),
                         ["build", "--output_groups=compilation_prerequisites_INTERNAL_",
                          "//dcfs:dcfs_lib"])

    def test_a_failed_prerequisite_build_is_a_tooling_error(self):
        with tempfile.TemporaryDirectory() as d:
            fake = os.path.join(d, "bazel")
            with open(fake, "w") as f:
                f.write("#!/bin/sh\necho boom >&2\nexit 1\n")
            os.chmod(fake, 0o755)
            old = os.environ["PATH"]
            os.environ["PATH"] = d + os.pathsep + old
            try:
                with redirect_stderr(io.StringIO()):
                    with self.assertRaises(SystemExit):
                        mutate.build_prerequisites(d, "//x:y")
            finally:
                os.environ["PATH"] = old


class ShardAndExitTest(unittest.TestCase):

    def test_shards_are_contiguous_disjoint_and_cover_everything(self):
        mutants = [{"id": i} for i in range(1, 278)]
        shards = [mutate.shard_of(mutants, k, 6) for k in range(1, 7)]
        ids = [m["id"] for sh in shards for m in sh]
        self.assertEqual(ids, list(range(1, 278)))
        self.assertTrue(all(len(sh) in (46, 47) for sh in shards))
        for sh in shards:
            self.assertEqual([m["id"] for m in sh],
                             list(range(sh[0]["id"], sh[0]["id"] + len(sh))))

    def test_exit_codes(self):
        killed, survived, error = ({"status": s} for s in ("killed", "survived", "error"))
        self.assertEqual(mutate.exit_code([killed], False), 0)
        self.assertEqual(mutate.exit_code([killed, survived], False), 0)   # a finding
        self.assertEqual(mutate.exit_code([killed, survived], True), 1)    # per push
        self.assertEqual(mutate.exit_code([survived, error], False), 2)    # tooling
        self.assertEqual(mutate.exit_code([survived, error], True), 2)
        self.assertEqual(mutate.exit_code([], True), 0)


class RunEndToEndTest(unittest.TestCase):
    """`run` over a canned mutants file in a throwaway git repository, with a
    fake bazel whose verdict is in the mutated file: KILL fails a test (exit 3),
    BAD is a tooling error (exit 2), anything else passes (survives)."""

    def setUp(self):
        self.dir = tempfile.mkdtemp(dir=os.environ.get("TEST_TMPDIR"))
        self.repo = os.path.join(self.dir, "repo")
        os.makedirs(os.path.join(self.repo, "dcfs"))
        with open(os.path.join(self.repo, "dcfs/a.cc"), "w") as f:
            f.write("int f() { return XXXX; }\n")
        for cmd in (["init", "-q"], ["add", "."],
                    ["-c", "user.name=t", "-c", "user.email=t@t", "commit", "-qm", "x"]):
            subprocess.run(["git", *cmd], cwd=self.repo, check=True)
        self.bazel = os.path.join(self.dir, "fakebazel")
        with open(self.bazel, "w") as f:
            f.write("#!/bin/sh\n"
                    "case \"$1\" in shutdown) exit 0;; esac\n"
                    "if grep -q KILL dcfs/a.cc; then echo 'FAIL: //dcfs:x_test'; exit 3; fi\n"
                    "if grep -q BAD dcfs/a.cc; then exit 2; fi\n"
                    "exit 0\n")
        os.chmod(self.bazel, os.stat(self.bazel).st_mode | stat.S_IXUSR)
        self.mutants = os.path.join(self.dir, "mutants.json")

    def write_mutants(self, replacements):
        ms = [{"id": i, "file": "dcfs/a.cc", "function": "f", "line": 1, "op": "negate-if",
               "start": 17, "end": 21, "replacement": r, "before": "XXXX"}
              for i, r in enumerate(replacements, 1)]
        with open(self.mutants, "w") as f:
            json.dump(ms, f)

    def run_tool(self, *extra):
        args = mutate.argparse.Namespace(
            workspace=self.repo, mutants=self.mutants, result=os.path.join(self.dir, "res.json"),
            sample=0, seed=1, only="", shard="", timeout=60, killers="//dcfs:x_test",
            fail_on_survivor="--fail-on-survivor" in extra, show_output=False,
            survivors_out=os.path.join(self.dir, "survivors.txt"), bazel=self.bazel)
        with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            return mutate.run(args)

    def test_a_survivor_is_a_finding_for_the_schedule_and_a_failure_per_push(self):
        self.write_mutants(["KILL", "ok", "KILL"])
        self.assertEqual(self.run_tool(), 0)
        survivors = open(os.path.join(self.dir, "survivors.txt")).read()
        self.assertIn("SURVIVOR dcfs/a.cc:1 in f", survivors)
        self.assertEqual(survivors.count("SURVIVOR"), 1)
        results = json.load(open(os.path.join(self.dir, "res.json")))
        self.assertEqual([r["status"] for r in results], ["killed", "survived", "killed"])
        self.assertEqual(self.run_tool("--fail-on-survivor"), 1)

    def test_no_survivor_passes_per_push(self):
        self.write_mutants(["KILL", "KILL"])
        self.assertEqual(self.run_tool("--fail-on-survivor"), 0)
        self.assertEqual(open(os.path.join(self.dir, "survivors.txt")).read(), "")

    def test_a_tooling_error_fails_both_modes(self):
        self.write_mutants(["KILL", "BAD"])
        self.assertEqual(self.run_tool(), 2)
        self.assertEqual(self.run_tool("--fail-on-survivor"), 2)

    def test_a_shard_runs_only_its_range(self):
        self.write_mutants(["ok", "ok", "ok", "ok"])
        args_ids = [m["id"] for m in mutate.shard_of(json.load(open(self.mutants)), 2, 2)]
        self.assertEqual(args_ids, [3, 4])


if __name__ == "__main__":
    unittest.main()
