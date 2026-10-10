"""Tests of the TLA+ mode of the mutation tester (mutate.py `--lang tla`,
plan step 12.13): the scope, `generate` and `changed` over a throwaway git
repository, the judging of Bazel's output (killed, survived, INVALID),
and `run` end to end on a tiny committed module (testdata/tiny.tla) with a
fake bazel that runs the real TLC on the mutated copy."""

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
import tla_operators as tla

# scope, arid, sets, tiny, tiny_cfg, java, jar, e2e: from the command line.
# e2e: "none" runs everything but the real-TLC end-to-end class, "only" runs
# that class alone (a medium test: each mutant is a JVM and a TLC run).
ARGS = {}


def read_text(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def read_json(path):
    return json.loads(read_text(path))


def git(repo, *cmd):
    subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@t",
                    *cmd], cwd=repo, check=True, capture_output=True)


class Repo(unittest.TestCase):
    """A throwaway git repository holding formal/tiny.tla and its cfg."""

    e2e = False

    @classmethod
    def setUpClass(cls):
        mode = ARGS.get("e2e", "all")
        if (mode == "none" and cls.e2e) or (mode == "only" and not cls.e2e):
            raise unittest.SkipTest("the other target runs it")

    def setUp(self):
        self.dir = tempfile.mkdtemp(dir=os.environ.get("TEST_TMPDIR"))
        self.repo = os.path.join(self.dir, "repo")
        os.makedirs(os.path.join(self.repo, "formal"))
        for name, key in (("tiny.tla", "tiny"), ("tiny.cfg", "tiny_cfg")):
            with open(os.path.join(self.repo, "formal", name), "w") as f:
                f.write(read_text(ARGS[key]))
        git(self.repo, "init", "-q")
        git(self.repo, "add", ".")
        git(self.repo, "commit", "-qm", "x")
        self.scope = os.path.join(self.dir, "tla_scope.txt")
        with open(self.scope, "w") as f:
            f.write("# c\nmodule formal/tiny.tla\nmodule formal/other.tla\n")
        self.equivalent = os.path.join(self.dir, "equivalent.txt")
        with open(self.equivalent, "w") as f:
            f.write("")

    def gen_args(self, **kw):
        a = dict(lang="tla", workspace=self.repo, scope=self.scope,
                 arid=ARGS["arid"], sets=ARGS["sets"],
                 equivalent=self.equivalent, module=[], seed=1,
                 per_function=0, all=False, changed="", max_mutants=0,
                 out=os.path.join(self.dir, "mutants.json"))
        a.update(kw)
        return mutate.argparse.Namespace(**a)

    def generate(self, **kw):
        with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            mutate.generate(self.gen_args(**kw))
        return read_json(os.path.join(self.dir, "mutants.json"))


class ScopeTest(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        if ARGS.get("e2e") == "only":
            raise unittest.SkipTest("the other target runs it")

    def test_the_real_scope_lists_the_models_and_not_the_harness(self):
        mods = mutate.read_tla_scope(ARGS["scope"])
        self.assertIn("formal/dcfs.tla", mods)
        self.assertIn("formal/Trace.tla", mods)
        self.assertFalse([m for m in mods if "/MC" in m or "known_bugs" in m])

    def test_a_malformed_line_is_a_tooling_error(self):
        with tempfile.NamedTemporaryFile("w", suffix=".txt") as f:
            f.write("file x y\n")
            f.flush()
            with self.assertRaises(SystemExit), redirect_stderr(
                    io.StringIO()):
                mutate.read_tla_scope(f.name)

    def test_the_default_killers_are_the_formal_tiers(self):
        self.assertEqual(mutate.default_killers("tla", "medium"),
                         "--config=fast //formal/...;"
                         "--config=presubmit //formal/...")
        self.assertEqual(mutate.default_killers("tla", "small"),
                         "--config=fast //formal/...")
        self.assertIn("//dcfs/...", mutate.default_killers("cpp", ""))


class GenerateTest(Repo):

    def test_generate_writes_tla_mutants_in_the_cpp_shape(self):
        ms = self.generate()
        self.assertGreaterEqual(len(ms), 5)
        self.assertEqual({m["lang"] for m in ms}, {"tla"})
        self.assertEqual({m["file"] for m in ms}, {"formal/tiny.tla"})
        self.assertEqual([m["id"] for m in ms], list(range(1, len(ms) + 1)))
        for field in ("function", "line", "operator", "op", "start", "end",
                      "replacement", "before", "key", "nth"):
            self.assertIn(field, ms[0])
        # A missing module of the scope (other.tla) is skipped, not fatal.

    def test_a_module_option_picks_one(self):
        ms = self.generate(module=["formal/tiny.tla"])
        self.assertTrue(ms)
        with self.assertRaises(SystemExit), redirect_stderr(io.StringIO()):
            self.generate(module=["formal/nonesuch.tla"])

    def test_the_sample_is_seeded_and_bounded(self):
        a = self.generate(per_function=2, seed=1)
        b = self.generate(per_function=2, seed=1)
        c = self.generate(per_function=2, seed=2)
        self.assertEqual([m["key"] for m in a], [m["key"] for m in b])
        self.assertNotEqual([m["key"] for m in a], [m["key"] for m in c])
        by = {}
        for m in a:
            by[m["function"]] = by.get(m["function"], 0) + 1
        self.assertLessEqual(max(by.values()), 2)

    def test_max_mutants_takes_a_seeded_budget_with_operators_in_turn(self):
        ms = self.generate(max_mutants=5)
        self.assertEqual(len(ms), 5)
        self.assertGreater(len({m["operator"] for m in ms}), 2)
        again = self.generate(max_mutants=5)
        self.assertEqual([m["key"] for m in ms], [m["key"] for m in again])

    def test_only_tla_equivalents_are_matched_and_warned_about(self):
        ms = self.generate(all=True)
        pick = next(m for m in ms if m["operator"] == "drop-guard")
        with open(self.equivalent, "w") as f:
            f.write(json.dumps({
                "file": pick["file"], "function": pick["function"],
                "op": pick["op"], "before": pick["before"],
                "after": pick["replacement"], "nth": pick["nth"],
                "reason": "a reason"}) + "\n")
            f.write(json.dumps({
                "file": "dcfs/x.cc", "function": "F", "op": "negate-if",
                "before": "a", "after": "b", "reason": "C++"}) + "\n")
        err = io.StringIO()
        with redirect_stdout(io.StringIO()), redirect_stderr(err):
            mutate.generate(self.gen_args())
        self.assertNotIn("matches no mutant", err.getvalue())
        got = read_json(os.path.join(self.dir, "mutants.json"))
        live, suppressed = mutate.split_suppressed(
            got, mutate.load_equivalent(self.equivalent))
        self.assertEqual(len(suppressed), 1)

    def test_changed_scopes_to_the_definitions_the_range_touches(self):
        path = os.path.join(self.repo, "formal/tiny.tla")
        text = read_text(path).replace("Logged == log <= 3",
                                       "Logged == log <= 2")
        with open(path, "w") as f:
            f.write(text)
        git(self.repo, "commit", "-qam", "touch Logged")
        ms = self.generate(changed="HEAD~1..HEAD")
        self.assertTrue(ms)
        self.assertEqual({m["function"] for m in ms}, {"Logged"})

    def test_a_range_that_touches_no_module_in_scope_has_no_mutants(self):
        with open(os.path.join(self.repo, "README"), "w") as f:
            f.write("x\n")
        git(self.repo, "add", ".")
        git(self.repo, "commit", "-qm", "readme")
        self.assertEqual(self.generate(changed="HEAD~1..HEAD"), [])


TLC_OUT = "==================== Test output for //formal:%s:\n%s\n"


class JudgeTest(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        if ARGS.get("e2e") == "only":
            raise unittest.SkipTest("the other target runs it")
    m = {"file": "formal/dcfs.tla"}

    def judge(self, rc, tail):
        return mutate.judge_tla(rc, tail, self.m)

    def test_all_passing_survives(self):
        self.assertEqual(self.judge(0, ""), ("survived", ""))

    def test_a_violation_found_where_none_is_expected_kills(self):
        tail = ("FAIL: //formal:small_test (see x)\n" + TLC_OUT % (
            "small_test", "Error: Invariant TriState is violated.\n"
            "tlc_test: TLC exited with status 12\n"
            "tlc_test: FAIL: expected TLC to find no error"))
        self.assertEqual(self.judge(3, tail), ("killed", "//formal:small_test"))

    def test_a_known_bug_that_no_longer_fails_is_killed_by_its_test(self):
        tail = ("FAIL: //formal:known_bug_x_test (see x)\n" + TLC_OUT % (
            "known_bug_x_test", "Model checking completed. No error has "
            "been found.\ntlc_test: TLC exited with status 0\n"
            "tlc_test: FAIL: expected TLC to report a violation"))
        self.assertEqual(self.judge(3, tail),
                         ("killed", "//formal:known_bug_x_test"))

    def test_a_deadlock_is_a_behavior_change_not_an_error(self):
        tail = ("FAIL: //formal:small_test (see x)\n" + TLC_OUT % (
            "small_test", "Error: Deadlock reached.\n"
            "tlc_test: TLC exited with status 11\n"))
        self.assertEqual(self.judge(3, tail)[0], "killed")

    def test_a_tlc_error_status_is_invalid(self):
        for status in (75, 255, 1):
            tail = ("FAIL: //formal:small_test (see x)\n" + TLC_OUT % (
                "small_test", "Error: Evaluating something failed.\n"
                "tlc_test: TLC exited with status %d\n" % status))
            self.assertEqual(self.judge(3, tail), ("invalid", ""), status)

    def test_a_parse_error_without_a_status_line_is_invalid(self):
        for text in ("Fatal errors while parsing TLA+ spec in file dcfs\n",
                     "Parsing or semantic analysis failed.\n",
                     "Error: Successor state is not completely specified\n",
                     "Error: TLC threw an unexpected exception.\n"):
            tail = ("FAIL: //formal:trace_x_test (see x)\n"
                    + TLC_OUT % ("trace_x_test", text))
            self.assertEqual(self.judge(3, tail), ("invalid", ""), text)

    def test_a_violation_message_is_not_an_error(self):
        tail = ("FAIL: //formal:trace_x_test (see x)\n" + TLC_OUT % (
            "trace_x_test", "Error: Invariant A is violated.\n"
            "Error: The behavior up to this point is:\n"
            "Error: Temporal properties were violated.\n"
            "Error: Action property B is violated.\n"))
        self.assertEqual(self.judge(3, tail)[0], "killed")

    def test_a_timeout_is_a_kill_by_that_test(self):
        tail = "TIMEOUT: //formal:large_x_test (see x)\n"
        self.assertEqual(self.judge(3, tail),
                         ("killed", "//formal:large_x_test"))

    def test_a_build_failure_is_a_tooling_error(self):
        self.assertEqual(self.judge(1, "ERROR: no such package")[0], "error")
        self.assertEqual(self.judge(2, "")[0], "error")

    def test_only_a_timeout_is_confirmed_by_a_rerun(self):
        self.assertTrue(mutate.needs_confirm("tla", "TIMEOUT: //a:b (x)"))
        self.assertFalse(mutate.needs_confirm("tla", "FAIL: //a:b (x)"))
        self.assertTrue(mutate.needs_confirm("cpp", "FAIL: //a:b (x)"))


class RunEndToEndTest(Repo):
    e2e = True

    """`run` on testdata/tiny.tla with a fake bazel that runs the real TLC
    on the mutated copy as a tlc_test would: `drop-guard` of `x < 3` is
    killed (the counter passes its bound), dropping the stuttering `Idle`
    step survives (nothing checks for deadlock), and a mutant TLC cannot
    parse is INVALID."""

    def setUp(self):
        super().setUp()
        # A `#!` line is limited to about 127 bytes and the interpreter's
        # path in a Bazel sandbox is longer (ENOEXEC): the script that is
        # executed is a short `/bin/sh` wrapper around the Python one.
        script = os.path.join(self.dir, "fakebazel.py")
        with open(script, "w") as f:
            f.write(FAKE_BAZEL.format(java=os.path.abspath(ARGS["java"]),
                                      jar=os.path.abspath(ARGS["jar"])))
        self.bazel = os.path.join(self.dir, "fakebazel")
        with open(self.bazel, "w") as f:
            f.write('#!/bin/sh\nexec "%s" "%s" "$@"\n' % (
                sys.executable, script))
        os.chmod(self.bazel, os.stat(self.bazel).st_mode | stat.S_IXUSR)

    def pick(self, ms, function, op, before=None):
        found = [m for m in ms if m["function"] == function
                 and m["op"].startswith(op)
                 and (before is None or m["before"] == before)]
        self.assertEqual(len(found), 1, (function, op, found))
        return found[0]

    def run_tool(self, mutants, **kw):
        path = os.path.join(self.dir, "pick.json")
        with open(path, "w") as f:
            json.dump(mutants, f)
        args = mutate.argparse.Namespace(
            workspace=self.repo, mutants=path,
            result=os.path.join(self.dir, "res.json"), sample=0, seed=1,
            only="", shard="", timeout=300, killers="//formal:tiny_test",
            fail_on_survivor=False, show_output=False,
            survivors_out=os.path.join(self.dir, "survivors.txt"),
            bazel=self.bazel, equivalent=self.equivalent, baseline=True,
            baseline_timeout=300,
            summary_out=os.path.join(self.dir, "summary.md"))
        for k, v in kw.items():
            setattr(args, k, v)
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            rc = mutate.run(args)
        results = read_json(args.result) if os.path.exists(args.result) \
            else []
        return rc, results, out.getvalue(), err.getvalue()

    def test_a_killed_a_surviving_and_an_invalid_mutant(self):
        ms = self.generate(all=True)
        guard = self.pick(ms, "Inc", "drop-guard", "x < 3")
        idle = self.pick(ms, "Next", "drop-disjunct", "Idle")
        logged = self.pick(ms, "Logged", "relational", "<=")
        garbage = dict(guard, replacement="TRUE /\\ (", op="garbage",
                       operator="negate")
        for i, m in enumerate([guard, idle, logged, garbage], 1):
            m["id"] = i
        rc, results, out, err = self.run_tool([guard, idle, logged, garbage])
        self.assertEqual(rc, 0, err)
        self.assertEqual([r["status"] for r in results],
                         ["killed", "survived", "survived", "invalid"])
        self.assertEqual(results[0]["killer"], "//formal:tiny_test")
        self.assertIn("SURVIVOR formal/tiny.tla", out)
        self.assertIn("in Next: drop-disjunct", out)
        self.assertIn("INVALID formal/tiny.tla", out)
        self.assertEqual(mutate.summarize(results)["total"],
                         {"killed": 1, "survived": 2, "invalid": 1})

    def test_the_module_is_restored_and_the_baseline_runs_unmutated(self):
        ms = self.generate(all=True)
        guard = self.pick(ms, "Inc", "drop-guard", "x < 3")
        guard["id"] = 1
        rc, results, _, _ = self.run_tool([guard])
        self.assertEqual(rc, 0)
        self.assertEqual(read_text(os.path.join(
            self.repo, "formal/tiny.tla")), read_text(ARGS["tiny"]))

    def test_a_failing_baseline_is_a_tooling_error(self):
        ms = self.generate(all=True)
        ms[0]["id"] = 1
        path = os.path.join(self.repo, "formal/tiny.tla")
        text = read_text(path).replace("=====", "Broken == (\n=====", 1)
        with open(path, "w") as f:
            f.write(text)
        git(self.repo, "commit", "-qam", "broken")
        rc, _, _, err = self.run_tool([ms[0]])
        self.assertEqual(rc, 2)
        self.assertIn("fail on the unmutated tree", err)

    def test_the_time_budget_leaves_the_rest_not_run(self):
        ms = self.generate(all=True)[:3]
        ticks = iter(range(0, 1000, 10))
        rc, results, _, err = self.run_tool(
            ms, deadline=5, clock=lambda: next(ticks), baseline=False)
        self.assertEqual(len(results), 1)
        self.assertIn("time budget spent", err)


FAKE_BAZEL = r'''"""A fake bazel: `test` runs the real TLC on formal/tiny.tla in the current
directory the way a tlc_test does (no error expected), printing what Bazel
prints for a failed test; shutdown and clean do nothing."""
import os, re, subprocess, sys, tempfile
args = [a for a in sys.argv[1:] if not a.startswith("--")]
if args[0] in ("shutdown", "clean"):
    sys.exit(0)
work = tempfile.mkdtemp()
for name in ("tiny.tla", "tiny.cfg"):
    with open("formal/" + name) as f, open(work + "/" + name, "w") as g:
        g.write(f.read())
p = subprocess.run(
    ["{java}", "-XX:+UseParallelGC", "-jar", "{jar}", "-workers", "1",
     "-metadir", work + "/states", "-config", "tiny.cfg", "tiny"],
    cwd=work, capture_output=True, text=True)
log = p.stdout + p.stderr
ok = p.returncode == 0 and "No error has been found" in log
if ok:
    sys.exit(0)
print("FAIL: //formal:tiny_test (see test.log)")
print("==================== Test output for //formal:tiny_test:")
print(log)
print("tlc_test: TLC exited with status %d" % p.returncode)
print("tlc_test: FAIL: expected TLC to find no error")
sys.exit(3)
'''


if __name__ == "__main__":
    rest, files = [], []
    for a in sys.argv[1:]:
        k, _, v = a.partition("=")
        if k in ("scope", "arid", "sets", "tiny", "tiny_cfg", "jar", "e2e"):
            ARGS[k] = v
        elif k == "java_files":  # `$(rootpaths ...)`: first is `k=PATH`
            files.append(v)
        elif "/" in a and not a.startswith("-"):
            files.append(a)
        else:
            rest.append(a)
    ARGS["java"] = next(f for f in files if f.endswith("/bin/java"))
    unittest.main(argv=[sys.argv[0]] + rest)
