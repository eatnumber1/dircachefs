"""Tests of the mutation tester (mutate.py): sampling, equivalents, reports,
the per-push mode, `run` end to end with a fake bazel. The operators are
tested on a real AST in operators_test.py."""

import io
import json
import os
import re
import signal
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
from contextlib import redirect_stderr, redirect_stdout

import mutate
import operators

ARGS = {}  # arid, equivalent: filled from the command line


def read_text(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def read_json(path):
    return json.loads(read_text(path))


def mutant(function="f", line=1, operator="relational", before="<",
           after="<=", nth=1, file="dcfs/a.cc", op=None, **kw):
    m = {"file": file, "function": function, "line": line,
         "operator": operator, "op": op or operator, "start": line * 10,
         "end": line * 10 + len(before), "replacement": after,
         "before": before, "nth": nth}
    m["key"] = mutate.stable_key(m)
    m.update(kw)
    return m


class ScopeTest(unittest.TestCase):

    def test_scope_rules(self):
        fn = {"kind": "FunctionDecl", "name": "f"}
        self.assertTrue(mutate.selected(fn, [("all", [])]))
        self.assertTrue(mutate.selected(fn, [("names", ["f", "g"])]))
        self.assertFalse(mutate.selected(fn, [("names", ["g"])]))
        self.assertFalse(mutate.selected(fn, [("calls", ["^Mark"])]))

    def test_the_real_scope_reads_and_covers_the_swept_files(self):
        here = os.path.dirname(ARGS["arid"])
        files = [e["file"] for e in mutate.read_scope(
            os.path.join(here, "scope.txt"))]
        self.assertIn("dcfs/dir_cache_fs.cc", files)
        self.assertIn("dcfs/backing.cc", files)

    def test_one_token_diff(self):
        self.assertEqual(operators.token_diff("a < b", "a <= b"),
                         ("<", "<="))
        self.assertEqual(operators.token_diff("x", "!(x)"), ("x", "!(x)"))
        self.assertEqual(operators.token_diff("m.End()", "(void)0"),
                         ("m.End()", "(void)0"))


class MacroSpellingTest(unittest.TestCase):
    """A node whose tokens spell in a header (a macro defined there) has
    header offsets: it must have no span in the .cc."""

    def doc(self, spelling_file):
        def loc(off, spelling_file):
            return {"spellingLoc": {"offset": off, "file": spelling_file,
                                    "tokLen": 3},
                    "expansionLoc": {"offset": 40, "file": "dcfs/a.cc",
                                     "tokLen": 6}}
        return {"kind": "FunctionDecl", "name": "f",
                "loc": {"offset": 0, "file": "dcfs/a.cc", "tokLen": 1},
                "range": {"begin": {"offset": 0, "tokLen": 1},
                          "end": {"offset": 90, "tokLen": 1}},
                "inner": [{"kind": "CompoundStmt", "inner": [
                    {"kind": "ReturnStmt", "range": {
                        "begin": loc(5, spelling_file),
                        "end": loc(9, spelling_file)}}]}]}

    def span_of_return(self, spelling_file):
        out = []
        doc = self.doc(spelling_file)
        mutate.Walker("dcfs/a.cc").visit(doc, out)
        ret = doc["inner"][0]["inner"][0]
        return operators.span(ret)

    def test_a_node_spelled_in_a_header_has_no_span(self):
        self.assertIsNone(self.span_of_return("dcfs/h.h"))

    def test_a_macro_argument_spelled_in_the_file_keeps_its_span(self):
        self.assertIsNotNone(self.span_of_return("dcfs/a.cc"))


class SamplingTest(unittest.TestCase):

    def mutants(self):
        out = []
        for fn in "ab":
            for line in range(1, 9):
                for operator in ("relational", "constant", "negate"):
                    out.append(mutant(function=fn, line=line,
                                      operator=operator,
                                      before="x%d" % line,
                                      after=operator, file="dcfs/%s.cc" % fn))
        return out

    def test_at_most_one_mutant_per_source_line(self):
        picked = mutate.sample(self.mutants(), seed=1)
        lines = [(m["file"], m["line"]) for m in picked]
        self.assertEqual(len(lines), len(set(lines)))
        self.assertEqual(len(picked), 16)

    def test_the_per_function_sample_is_bounded_and_covers_operators(self):
        picked = mutate.sample(self.mutants(), seed=1, per_function=3)
        for fn in "ab":
            mine = [m for m in picked if m["function"] == fn]
            self.assertEqual(len(mine), 3)
        # The operators take turns: with the lines each holding one pick,
        # a cut of three takes one of each when each is available.
        by_operator = {m["operator"] for m in picked if m["function"] == "a"}
        self.assertGreaterEqual(len(by_operator), 2)

    def test_different_seeds_pick_different_operators_per_function(self):
        # One function with every operator: a cut of one must not always
        # be the alphabetically first operator.
        ms = [mutant(function="f", line=i, operator=op, before="b%d" % i,
                     after=op)
              for i, op in enumerate(
                  ("constant", "enum-swap", "logical", "negate",
                   "relational", "status-return", "swap-args"), 1)]
        picked = {mutate.sample(ms, seed=s, per_function=1)[0]["operator"]
                  for s in range(1, 30)}
        self.assertGreaterEqual(len(picked), 4)
        self.assertIn("status-return", picked)

    def test_the_same_seed_gives_the_same_sample_and_another_differs(self):
        a = mutate.sample(self.mutants(), seed=1, per_function=3)
        b = mutate.sample(self.mutants(), seed=1, per_function=3)
        c = mutate.sample(self.mutants(), seed=2, per_function=3)
        self.assertEqual([m["key"] for m in a], [m["key"] for m in b])
        self.assertNotEqual([m["key"] for m in a], [m["key"] for m in c])

    def test_a_mutant_keeps_its_rank_when_code_elsewhere_moves(self):
        m = mutant()
        moved = dict(m, line=99, start=990, end=991)
        self.assertEqual(mutate.rank(1, m), mutate.rank(1, moved))
        self.assertNotEqual(mutate.rank(1, m), mutate.rank(2, m))

    def test_the_budget_picks_deterministically_and_reports_the_surplus(self):
        ms = mutate.sample(self.mutants(), seed=1)
        chosen, not_run = mutate.pick_budget(ms, 5, seed=1)
        again, _ = mutate.pick_budget(ms, 5, seed=1)
        self.assertEqual(len(chosen), 5)
        self.assertEqual(not_run, len(ms) - 5)
        self.assertEqual(chosen, again)
        self.assertGreaterEqual(len({m["operator"] for m in chosen}), 3)

    def test_a_budget_larger_than_the_mutants_runs_them_all(self):
        ms = mutate.sample(self.mutants(), seed=1)
        chosen, not_run = mutate.pick_budget(ms, 1000, seed=1)
        self.assertEqual((len(chosen), not_run), (len(ms), 0))


class EquivalentTest(unittest.TestCase):

    def entry(self, **kw):
        e = {"file": "dcfs/a.cc", "function": "f", "op": "delete-call End",
             "before": "m.End()", "after": "(void)0", "reason": "why"}
        e.update(kw)
        return json.dumps(e)

    def test_an_entry_without_a_reason_is_rejected(self):
        for bad in (self.entry(reason=""), self.entry(reason="  "),
                    '{"file": "a", "function": "f", "op": "o", '
                    '"before": "b", "after": "c"}'):
            with self.assertRaises(mutate.EquivalentError):
                mutate.parse_equivalent(bad)

    def test_a_malformed_line_is_rejected(self):
        with self.assertRaises(mutate.EquivalentError):
            mutate.parse_equivalent("not json")

    def test_comments_and_blank_lines_are_skipped(self):
        self.assertEqual(
            len(mutate.parse_equivalent("# c\n\n" + self.entry() + "\n")), 1)

    def test_the_real_file_parses_and_every_entry_has_a_reason(self):
        with open(ARGS["equivalent"], encoding="utf-8") as f:
            entries = mutate.parse_equivalent(f.read())
        for e in entries:
            self.assertTrue(e["reason"].strip())

    def test_a_mutant_is_matched_by_text_not_by_line(self):
        entries = mutate.parse_equivalent(self.entry())
        m = mutant(op="delete-call End", before="m.End()", after="(void)0",
                   line=5)
        self.assertEqual(mutate.suppression(m, entries), "why")
        self.assertEqual(
            mutate.suppression(dict(m, line=500), entries), "why")
        self.assertIsNone(mutate.suppression(dict(m, function="g"), entries))
        self.assertIsNone(mutate.suppression(dict(m, op="negate-if"),
                                             entries))
        self.assertIsNone(
            mutate.suppression(dict(m, replacement="x"), entries))

    def test_nth_names_one_of_several_identical_mutants(self):
        entries = mutate.parse_equivalent(self.entry(nth=2))
        m = mutant(op="delete-call End", before="m.End()", after="(void)0")
        self.assertIsNone(mutate.suppression(dict(m, nth=1), entries))
        self.assertEqual(mutate.suppression(dict(m, nth=2), entries), "why")

    def test_split_suppressed(self):
        entries = mutate.parse_equivalent(self.entry())
        eq = mutant(op="delete-call End", before="m.End()", after="(void)0")
        other = mutant()
        live, suppressed = mutate.split_suppressed([eq, other], entries)
        self.assertEqual(live, [other])
        self.assertEqual([m["reason"] for m in suppressed], ["why"])


class PlanTest(unittest.TestCase):

    def entries(self, **kw):
        e = {"file": "dcfs/a.cc", "function": "f", "op": "relational",
             "before": "<", "after": "<=", "reason": "r"}
        e.update(kw)
        return [e]

    def test_equivalents_do_not_win_lines_or_slots(self):
        eq = mutant(line=1)                       # the equivalent one
        other = mutant(line=1, before=">", after=">=", op="relational")
        entries = self.entries()
        for seed in range(1, 40):                 # whichever ranks first
            live, suppressed = mutate.plan([eq, other], entries, seed, 0)
            self.assertEqual([m["before"] for m in live], [">"], seed)
            self.assertEqual(len(suppressed), 1)

    def test_an_entry_that_matches_nothing_or_several_is_warned_about(self):
        a = mutant(line=1, nth=1)
        b = mutant(line=2, nth=2)
        b["key"] = a["key"]
        ents = mutate.parse_equivalent("\n".join(
            json.dumps(e) for e in
            self.entries() + self.entries(function="gone")
            + self.entries(nth=2)))
        warnings = mutate.equivalent_warnings(ents, [a, b])
        self.assertEqual(len(warnings), 2)
        self.assertIn("matches 2 mutants", warnings[0])
        self.assertIn("matches no mutant", warnings[1])


class ReportTest(unittest.TestCase):

    def results(self):
        def r(status, operator, line=1, fn="f", seconds=60, before="a < b",
              after="a <= b"):
            return dict(mutant(function=fn, line=line, operator=operator,
                               before=before, after=after, id=line),
                        status=status, seconds=seconds, killer="")
        return [r("killed", "relational", 1), r("survived", "relational", 2),
                r("survived", "relational", 3, fn="g"),
                r("invalid", "constant", 4), r("killed", "constant", 5),
                r("suppressed", "constant", 6, seconds=0)]

    def test_the_summary_counts_per_operator_and_per_hour(self):
        s = mutate.summarize(self.results())
        self.assertEqual(s["operators"]["relational"],
                         {"killed": 1, "survived": 2})
        self.assertEqual(s["operators"]["constant"],
                         {"invalid": 1, "killed": 1, "suppressed": 1})
        self.assertEqual(s["total"]["suppressed"], 1)
        self.assertEqual(s["run"], 5)       # suppressed ones did not run
        self.assertEqual(s["per_hour"], 60.0)  # 5 mutants in 300 s

    def test_a_wall_time_replaces_the_sum_of_mutant_times(self):
        s = mutate.summarize(self.results(), wall=600)
        self.assertEqual(s["per_hour"], 30.0)

    def test_the_table_has_a_row_per_operator_and_the_rate(self):
        text = mutate.format_summary(mutate.summarize(self.results()),
                                     not_run=4)
        self.assertIn("| relational | 1 | 2 | 0 | 0 | 0 | 0 | 3 |", text)
        self.assertIn("| constant | 1 | 0 | 1 | 1 | 0 | 0 | 3 |", text)
        self.assertIn("| **all** | 2 | 2 | 1 | 1 | 0 | 0 | 6 |", text)
        self.assertIn("60.0 mutants per hour", text)
        self.assertIn("4 not run", text)

    def test_survivors_are_grouped_by_function_with_a_one_token_diff(self):
        lines = mutate.format_survivors(self.results())
        self.assertEqual(lines, [
            "== dcfs/a.cc f: 1 survivor",
            "SURVIVOR dcfs/a.cc:2 in f: relational: `<` -> `<=`",
            "== dcfs/a.cc g: 1 survivor",
            "SURVIVOR dcfs/a.cc:3 in g: relational: `<` -> `<=`"])

    def test_a_result_of_an_older_run_has_no_operator_field(self):
        r = dict(self.results()[0])
        del r["operator"]
        r["op"] = "negate-if"
        self.assertEqual(list(mutate.summarize([r])["operators"]),
                         ["negate-if"])

    def test_report_merges_result_files(self):
        with tempfile.TemporaryDirectory() as d:
            paths = []
            for i, rs in enumerate((self.results()[:3], self.results()[3:])):
                paths.append(os.path.join(d, "r%d.json" % i))
                with open(paths[-1], "w") as f:
                    json.dump(rs, f)
            out = io.StringIO()
            with redirect_stdout(out):
                self.assertEqual(
                    mutate.report(mutate.argparse.Namespace(results=paths)),
                    0)
        self.assertIn("| **all** | 2 | 2 | 1 | 1 | 0 | 0 | 6 |", out.getvalue())
        self.assertIn("Survivors by function (2)", out.getvalue())


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
        self.assertEqual(mutate.parse_hunks(DIFF),
                         [(10, 12), (41, 41), (60, 61)])

    def test_a_function_is_touched_when_a_hunk_overlaps_its_lines(self):
        hunks = mutate.parse_hunks(DIFF)
        # the first line of a hunk
        self.assertTrue(mutate.touches((1, 10), hunks))
        self.assertTrue(mutate.touches((12, 30), hunks))   # the last
        self.assertTrue(mutate.touches((41, 41), hunks))
        # a deletion after line 60
        self.assertTrue(mutate.touches((55, 60), hunks))
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
        self.assertEqual(mutate.normalize_range("%s..%s" % ("0" * 40, tip)),
                         "abc123~1..abc123")
        self.assertEqual(mutate.normalize_range("..abc123"), "abc123~1..abc123")
        self.assertEqual(mutate.normalize_range("a..b"), "a..b")

    def test_a_range_with_no_hunks_selects_nothing(self):
        self.assertEqual(mutate.parse_hunks(""), [])
        self.assertFalse(mutate.touches((1, 100), []))


class PrerequisitesTest(unittest.TestCase):
    """The AST dump needs the generated headers (libfuse_config.h) first."""

    def test_the_build_asks_for_the_compile_prerequisites_of_the_target(self):
        self.assertEqual(mutate.prerequisites_args("//dcfs:dcfs_lib"),
                         ["build",
                          "--output_groups=compilation_prerequisites_INTERNAL_",
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
                    with self.assertRaises(SystemExit) as raised:
                        mutate.build_prerequisites(d, "//x:y")
                self.assertEqual(raised.exception.code, 2)
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
        killed, survived, error = ({"status": s} for s in (
            "killed", "survived", "error"))
        self.assertEqual(mutate.exit_code([killed], False), 0)
        # a finding
        self.assertEqual(mutate.exit_code([killed, survived], False), 0)
        # per push
        self.assertEqual(mutate.exit_code([killed, survived], True), 1)
        # tooling
        self.assertEqual(mutate.exit_code([survived, error], False), 2)
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
                    ["-c", "user.name=t", "-c", "user.email=t@t", "commit",
                     "-qm", "x"]):
            subprocess.run(["git", *cmd], cwd=self.repo, check=True)
        self.bazel = os.path.join(self.dir, "fakebazel")
        self.log = os.path.join(self.dir, "bazel.log")
        with open(self.bazel, "w") as f:
            f.write(
                "#!/bin/sh\n"
                "echo \"$@\" >> %s\n"
                "while case \"$1\" in --*) true;; *) false;; esac; do "
                "shift; done\n"
                "case \"$1\" in shutdown|clean) exit 0;; esac\n"
                "if grep -q KILL dcfs/a.cc; then "
                "echo 'FAIL: //dcfs:x_test'; exit 3; fi\n"
                "if grep -q FLAKY dcfs/a.cc; then "
                "case \"$*\" in *--nocache_test_results*) exit 0;; esac; "
                "echo 'FAIL: //dcfs:x_test'; exit 3; fi\n"
                "if grep -q SLOW dcfs/a.cc; then sleep 5; fi\n"
                "if grep -q INVALID dcfs/a.cc; then "
                "echo 'dcfs/a.cc:1:20: error: expected expression'; "
                "exit 1; fi\n"
                "if grep -q BUILDFAIL dcfs/a.cc; then "
                "echo 'other/b.cc:3:1: error: boom'; exit 1; fi\n"
                "if grep -q BAD dcfs/a.cc; then exit 2; fi\n"
                "exit 0\n" % self.log)
        os.chmod(self.bazel, os.stat(self.bazel).st_mode | stat.S_IXUSR)
        self.mutants = os.path.join(self.dir, "mutants.json")
        self.equivalent = ""
        self.timeout = 60

    def write_mutants(self, replacements):
        ms = [{"id": i, "file": "dcfs/a.cc", "function": "f", "line": 1,
               "op": "negate-if", "start": 17, "end": 21, "replacement": r,
               "before": "XXXX"}
              for i, r in enumerate(replacements, 1)]
        with open(self.mutants, "w") as f:
            json.dump(ms, f)

    def run_tool(self, *extra):
        args = self.args(*extra)
        with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            return mutate.run(args)

    def args(self, *extra):
        return mutate.argparse.Namespace(
            workspace=self.repo, mutants=self.mutants,
            result=os.path.join(self.dir, "res.json"),
            sample=0, seed=1, only="", shard="", timeout=self.timeout,
            killers="//dcfs:x_test",
            fail_on_survivor="--fail-on-survivor" in extra, show_output=False,
            survivors_out=os.path.join(self.dir, "survivors.txt"),
            bazel=self.bazel, equivalent=self.equivalent,
            baseline=True, baseline_timeout=60,
            summary_out=os.path.join(self.dir, "summary.md"))

    def test_a_survivor_is_a_finding_on_the_schedule_a_failure_per_push(self):
        self.write_mutants(["KILL", "ok", "KILL"])
        self.assertEqual(self.run_tool(), 0)
        survivors = read_text(os.path.join(self.dir, "survivors.txt"))
        self.assertIn("SURVIVOR dcfs/a.cc:1 in f", survivors)
        self.assertEqual(survivors.count("SURVIVOR"), 1)
        results = read_json(os.path.join(self.dir, "res.json"))
        self.assertEqual([r["status"] for r in results],
                         ["killed", "survived", "killed"])
        self.assertEqual(self.run_tool("--fail-on-survivor"), 1)

    def test_no_survivor_passes_per_push(self):
        self.write_mutants(["KILL", "KILL"])
        self.assertEqual(self.run_tool("--fail-on-survivor"), 0)
        self.assertEqual(read_text(os.path.join(self.dir, "survivors.txt")), "")

    def test_a_tooling_error_fails_both_modes(self):
        self.write_mutants(["KILL", "BAD"])
        self.assertEqual(self.run_tool(), 2)
        self.assertEqual(self.run_tool("--fail-on-survivor"), 2)

    def test_a_suppressed_mutant_is_not_run_and_counted_apart(self):
        self.write_mutants(["KILL", "ok", "BAD"])
        path = os.path.join(self.dir, "equivalent.txt")
        with open(path, "w") as f:
            f.write(json.dumps({
                "file": "dcfs/a.cc", "function": "f", "op": "negate-if",
                "before": "XXXX", "after": "BAD", "reason": "never runs"}))
        self.equivalent = path
        self.assertEqual(self.run_tool("--fail-on-survivor"), 1)  # not 2
        results = read_json(os.path.join(self.dir, "res.json"))
        self.assertEqual(sorted(r["status"] for r in results),
                         ["killed", "suppressed", "survived"])
        suppressed = [r for r in results if r["status"] == "suppressed"][0]
        self.assertEqual(suppressed["reason"], "never runs")
        summary = read_text(os.path.join(self.dir, "summary.md"))
        self.assertIn("| negate-if | 1 | 1 | 0 | 1 | 0 | 0 | 3 |", summary)

    def test_the_time_budget_leaves_the_remaining_mutants_not_run(self):
        self.write_mutants(["KILL", "KILL", "KILL", "KILL"])
        ticks = iter(range(0, 1000, 10))  # a fake clock: 10 s per look
        extra = {"deadline": 25, "clock": lambda: next(ticks)}
        args = self.args()
        for k, v in extra.items():
            setattr(args, k, v)
        with redirect_stdout(io.StringIO()), redirect_stderr(
                io.StringIO()) as err:
            code = mutate.run(args)
        self.assertEqual(code, 0)  # not an error
        results = read_json(os.path.join(self.dir, "res.json"))
        self.assertEqual(len(results), 3)  # looks at 0, 10, 20; 30 > 25
        self.assertIn("1 not run (time budget)", err.getvalue())

    def statuses(self):
        results = read_json(os.path.join(self.dir, "res.json"))
        return [r["status"] for r in results]

    def test_a_kill_that_passes_when_rerun_is_flaky(self):
        self.write_mutants(["KILL", "FLAKY"])
        self.assertEqual(self.run_tool("--fail-on-survivor"), 0)
        self.assertEqual(self.statuses(), ["killed", "flaky"])
        self.assertIn("--nocache_test_results", read_text(self.log))

    def test_a_build_error_in_the_mutated_file_is_invalid(self):
        self.write_mutants(["INVALID"])
        self.assertEqual(self.run_tool(), 0)
        self.assertEqual(self.statuses(), ["invalid"])

    def test_a_build_error_elsewhere_is_a_tooling_error(self):
        self.write_mutants(["BUILDFAIL"])
        self.assertEqual(self.run_tool(), 2)
        self.assertEqual(self.statuses(), ["error"])

    def test_a_timeout_is_a_kill_and_is_not_rerun(self):
        self.write_mutants(["SLOW"])
        self.timeout = 1
        self.assertEqual(self.run_tool(), 0)
        self.assertEqual(self.statuses(), ["killed"])
        self.assertNotIn("--nocache_test_results", read_text(self.log))

    def test_the_output_base_and_scratch_tree_are_removed(self):
        self.write_mutants(["KILL"])
        self.run_tool()
        log = read_text(self.log)
        base = re.search(r"--output_base=(\S+)", log).group(1)
        self.assertIn("clean --expunge", log)
        self.assertFalse(os.path.exists(os.path.dirname(base)))

    def test_a_failure_while_copying_still_removes_the_scratch_tree(self):
        made = []
        real = tempfile.mkdtemp

        def mkdtemp(*a, **kw):
            made.append(real(*a, **kw))
            return made[-1]

        def broken(workspace, dest):
            raise OSError("disk full")
        self.write_mutants(["KILL"])
        with mock.patch.object(tempfile, "mkdtemp", mkdtemp), \
                mock.patch.object(mutate, "copy_tree", broken):
            with self.assertRaises(OSError):
                self.run_tool()
        self.assertTrue(made)
        self.assertFalse(os.path.exists(made[0]))

    def test_sigterm_becomes_an_exit_so_that_cleanup_runs(self):
        old = signal.getsignal(signal.SIGTERM)
        try:
            mutate.install_sigterm()
            with self.assertRaises(SystemExit):
                signal.raise_signal(signal.SIGTERM)
        finally:
            signal.signal(signal.SIGTERM, old)

    def test_a_failing_baseline_is_a_tooling_error_and_runs_no_mutant(self):
        # The unmutated tree fails the killers: no verdict would mean
        # anything (every mutant would look killed).
        with open(os.path.join(self.repo, "dcfs/a.cc"), "w") as f:
            f.write("int f() { return KILL; }\n")
        self.write_mutants(["ok", "ok"])
        self.assertEqual(self.run_tool(), 2)
        self.assertFalse(os.path.exists(os.path.join(self.dir, "res.json")))

    def test_a_shard_runs_only_its_range(self):
        self.write_mutants(["ok", "ok", "ok", "ok"])
        args_ids = [m["id"] for m in mutate.shard_of(
            read_json(self.mutants), 2, 2)]
        self.assertEqual(args_ids, [3, 4])


if __name__ == "__main__":
    rest = []
    for arg in sys.argv[1:]:
        key, eq, value = arg.partition("=")
        if eq and key in ("arid", "equivalent"):
            ARGS[key] = value
        else:
            rest.append(arg)
    unittest.main(argv=[sys.argv[0]] + rest)
