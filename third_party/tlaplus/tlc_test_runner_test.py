# SPDX-License-Identifier: MIT
"""Tests of the tlc_test runner (tlc_test_runner.sh.tpl, plan step 12.28):
the template is expanded with a fake `java` that stands in for TLC, so the
test controls what "TLC" prints and when it exits."""

import os
import select
import signal
import stat
import subprocess
import sys
import tempfile
import unittest

# The template's path, from the command line.
TEMPLATE = None

# A hang guard, not a verdict: the verdict is the line arriving. Absent
# output cannot signal, so the guard only bounds how long the test waits
# before it fails and kills its process group, which Bazel's own timeout
# kill would orphan.
HANG_GUARD_SECONDS = 30

PROGRESS = "Progress(7) at 2026-10-20: 1234 states generated"


class RunnerTest(unittest.TestCase):

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.proc = None

    def tearDown(self):
        if self.proc is not None and self.proc.poll() is None:
            os.killpg(self.proc.pid, signal.SIGKILL)
            self.proc.wait()

    def start(self, fake_java, expect=""):
        """Starts the runner with `fake_java` (a bash body) as its java.

        `expect` is the template's @@EXPECT@@: the violation the run must
        report, empty for none.
        """
        root = self.tmp.name
        for name in ("tlc.jar", "m.tla", "m.cfg"):
            with open(os.path.join(root, name), "w", encoding="utf-8") as f:
                f.write("\n")
        java = os.path.join(root, "java")
        with open(java, "w", encoding="utf-8") as f:
            f.write("#!/bin/bash\n" + fake_java)
        os.chmod(java, os.stat(java).st_mode | stat.S_IXUSR)
        with open(TEMPLATE, encoding="utf-8") as f:
            text = f.read()
        for key, value in {
                "@@JAVA@@": "java", "@@JAR@@": "tlc.jar",
                "@@SPEC@@": "m.tla", "@@CONFIG@@": "m.cfg",
                "@@SRCS@@": "", "@@EXPECT@@": expect, "@@TLC_ARGS@@": "",
                "@@WORKERS@@": "1", "@@COVERAGE@@": ""}.items():
            text = text.replace(key, value)
        script = os.path.join(root, "runner.sh")
        with open(script, "w", encoding="utf-8") as f:
            f.write(text)
        env = dict(os.environ, TEST_TMPDIR=os.path.join(root, "t"))
        os.mkdir(env["TEST_TMPDIR"])
        self.proc = subprocess.Popen(
            ["/bin/bash", script], cwd=root, env=env, stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            start_new_session=True)
        # Cleanups run after tearDown, which has waited for the process.
        self.addCleanup(self.proc.stdout.close)
        self.addCleanup(self.proc.stdin.close)

    def read_until(self, marker):
        """Reads stdout until `marker` has arrived; returns all of it read.

        That includes what came after the marker. Fails the test at the
        hang guard or at EOF.
        """
        seen = b""
        while marker.encode() not in seen:
            ready, _, _ = select.select(
                [self.proc.stdout], [], [], HANG_GUARD_SECONDS)
            self.assertTrue(
                ready, "no output containing %r before the hang guard; "
                "got %r" % (marker, seen))
            chunk = os.read(self.proc.stdout.fileno(), 4096)
            self.assertTrue(chunk, "runner exited before %r; got %r" % (
                marker, seen))
            seen += chunk
        return seen.decode()

    def finish(self):
        out = self.proc.stdout.read().decode()
        return self.proc.wait(), out

    def test_output_reaches_stdout_while_tlc_runs(self):
        # TLC prints a line and keeps running (it blocks reading its
        # stdin, which the test never writes); the line must reach the
        # runner's stdout before TLC exits, as it does when Bazel kills a
        # timed-out test.
        self.start("echo '%s'\nread -r _\n" % PROGRESS)
        self.assertIn(PROGRESS, self.read_until(PROGRESS))

    def test_clean_run_passes_and_prints_output_once(self):
        self.start("echo 'Model checking completed. No error has been "
                   "found.'\n")
        status, out = self.finish()
        self.assertEqual(status, 0, out)
        self.assertEqual(out.count("Model checking completed."), 1, out)
        self.assertIn("tlc_test: TLC exited with status 0", out)
        self.assertIn("tlc_test: PASS: no error found", out)

    def test_expected_violation_keeps_tlc_exit_status(self):
        self.start("echo 'Invariant Inv is violated.'\nexit 12\n",
                   expect="Invariant Inv is violated")
        status, out = self.finish()
        self.assertEqual(status, 0, out)
        self.assertIn("tlc_test: TLC exited with status 12", out)
        self.assertIn("tlc_test: PASS: found the expected violation", out)

    def test_other_exit_status_fails(self):
        self.start("echo 'Invariant Inv is violated.'\nexit 75\n",
                   expect="Invariant Inv is violated")
        status, out = self.finish()
        self.assertNotEqual(status, 0, out)
        self.assertIn("tlc_test: TLC exited with status 75", out)
        self.assertIn("got 75", out)


if __name__ == "__main__":
    TEMPLATE, = sys.argv[1:2]
    unittest.main(argv=sys.argv[:1])
