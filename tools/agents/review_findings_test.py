# SPDX-License-Identifier: MIT
"""Tests for review_findings.py."""

import io
import os
import tempfile
import unittest

import review_findings


class ReviewFindingsTest(unittest.TestCase):

  def setUp(self):
    self.temp_dir = tempfile.TemporaryDirectory()
    self.addCleanup(self.temp_dir.cleanup)
    self.repo_dir = self.temp_dir.name
    self.tsv_dir = os.path.join(self.repo_dir, "docs", "plan")
    os.makedirs(self.tsv_dir, exist_ok=True)
    self.tsv_path = os.path.join(self.tsv_dir, "review-findings.tsv")

  def test_find_main_repo(self):
    self.assertEqual(
        review_findings.find_main_repo(
            environ={"DCFS_MAIN_REPO": "/custom/path"}
        ),
        "/custom/path",
    )
    self.assertEqual(
        review_findings.find_main_repo(
            environ={"DCFS_MAIN_CHECKOUT": "/custom/checkout"}
        ),
        "/custom/checkout",
    )
    self.assertEqual(
        review_findings.find_main_repo(environ={"HOME": "/home/alice"}),
        "/home/alice/Sources/dircachefs",
    )

  def test_add_valid_findings_block(self):
    # Initialize TSV with header
    review_findings.ensure_tsv_file(self.tsv_path)

    findings_block = (
        "blocker | correctness | docs/style.md 1.6 | dcfs/foo.cc:10 | Return"
        " error on failure | no | open\n"
        "should | test-gap | AGENTS.md | dcfs/foo_test.cc:20 | Add test for"
        " error path | could | open\n"
        "nit | naming | docs/style.md 2 | dcfs/foo.h:5 | Rename x to count |"
        " checked | fixed 1234567\n"
    )

    stdin = io.StringIO(findings_block)
    stdout = io.StringIO()
    stderr = io.StringIO()

    exit_code = review_findings.main(
        [
            "add",
            "--step",
            "22.5",
            "--round",
            "2",
            "--reviewer",
            "dcfs-reviewer/claude-opus-5-5/high",
            "--date",
            "2026-10-18",
            "--repo",
            self.repo_dir,
        ],
        stdin=stdin,
        stdout=stdout,
        stderr=stderr,
    )
    self.assertEqual(exit_code, 0)

    with open(self.tsv_path, "r", encoding="utf-8") as f:
      lines = f.readlines()

    self.assertEqual(len(lines), 4)  # header + 3 rows
    self.assertEqual(
        lines[0],
        "date\tstep\tround\treviewer\tseverity\tcategory\t"
        "rule\tfile\tsummary\tmechanizable\toutcome\n",
    )
    self.assertEqual(
        lines[1],
        "2026-10-18\t22.5\t2\tdcfs-reviewer/claude-opus-5-5/high\tblocker\t"
        "correctness\tdocs/style.md 1.6\tdcfs/foo.cc:10\t"
        "Return error on failure\tno\topen\n",
    )
    self.assertEqual(
        lines[2],
        "2026-10-18\t22.5\t2\tdcfs-reviewer/claude-opus-5-5/high\tshould\t"
        "test-gap\tAGENTS.md\tdcfs/foo_test.cc:20\t"
        "Add test for error path\tcould\topen\n",
    )
    self.assertEqual(
        lines[3],
        "2026-10-18\t22.5\t2\tdcfs-reviewer/claude-opus-5-5/high\tnit\t"
        "naming\tdocs/style.md 2\tdcfs/foo.h:5\t"
        "Rename x to count\tchecked\tfixed 1234567\n",
    )

  def test_add_none_finding(self):
    findings_block = "none | | | | clean | no | open\n"
    stdin = io.StringIO(findings_block)
    exit_code = review_findings.main(
        [
            "add",
            "--step",
            "agents",
            "--round",
            "1",
            "--reviewer",
            "gemini-3.8-flash-medium",
            "--date",
            "2026-10-17",
            "--repo",
            self.repo_dir,
        ],
        stdin=stdin,
    )
    self.assertEqual(exit_code, 0)

    with open(self.tsv_path, "r", encoding="utf-8") as f:
      lines = f.readlines()
    self.assertEqual(len(lines), 2)  # header + 1 row
    self.assertEqual(
        lines[1],
        "2026-10-17\tagents\t1\tgemini-3.8-flash-medium\tnone\t\t\t\tclean\t"
        "no\topen\n",
    )

  def test_add_rejects_tab(self):
    findings_block = (
        "nit | naming | docs/style.md\t2 | dcfs/foo.h:5 | Rename | no | open\n"
    )
    stdin = io.StringIO(findings_block)
    stderr = io.StringIO()
    exit_code = review_findings.main(
        [
            "add",
            "--step",
            "22.5",
            "--round",
            "1",
            "--reviewer",
            "reviewer",
            "--date",
            "2026-10-17",
            "--repo",
            self.repo_dir,
        ],
        stdin=stdin,
        stderr=stderr,
    )
    self.assertNotEqual(exit_code, 0)
    self.assertIn("tab", stderr.getvalue().lower())

  def test_add_invalid_severity(self):
    findings_block = (
        "major | correctness | docs/style.md 1.6 | dcfs/foo.cc:10 | Bad | no |"
        " open\n"
    )
    stdin = io.StringIO(findings_block)
    stderr = io.StringIO()
    exit_code = review_findings.main(
        [
            "add",
            "--step",
            "22.5",
            "--round",
            "1",
            "--reviewer",
            "reviewer",
            "--date",
            "2026-10-17",
            "--repo",
            self.repo_dir,
        ],
        stdin=stdin,
        stderr=stderr,
    )
    self.assertNotEqual(exit_code, 0)
    self.assertIn("severity", stderr.getvalue().lower())

  def test_add_invalid_category(self):
    findings_block = (
        "should | bad-category | docs/style.md 1.6 | dcfs/foo.cc:10 | Bad | no"
        " | open\n"
    )
    stdin = io.StringIO(findings_block)
    stderr = io.StringIO()
    exit_code = review_findings.main(
        [
            "add",
            "--step",
            "22.5",
            "--round",
            "1",
            "--reviewer",
            "reviewer",
            "--date",
            "2026-10-17",
            "--repo",
            self.repo_dir,
        ],
        stdin=stdin,
        stderr=stderr,
    )
    self.assertNotEqual(exit_code, 0)
    self.assertIn("category", stderr.getvalue().lower())

  def test_add_invalid_mechanizable(self):
    findings_block = (
        "should | correctness | docs/style.md 1.6 | dcfs/foo.cc:10 | Bad |"
        " maybe | open\n"
    )
    stdin = io.StringIO(findings_block)
    stderr = io.StringIO()
    exit_code = review_findings.main(
        [
            "add",
            "--step",
            "22.5",
            "--round",
            "1",
            "--reviewer",
            "reviewer",
            "--date",
            "2026-10-17",
            "--repo",
            self.repo_dir,
        ],
        stdin=stdin,
        stderr=stderr,
    )
    self.assertNotEqual(exit_code, 0)
    self.assertIn("mechanizable", stderr.getvalue().lower())

  def test_add_wrong_field_count(self):
    findings_block = "should | correctness | too | few\n"
    stdin = io.StringIO(findings_block)
    stderr = io.StringIO()
    exit_code = review_findings.main(
        [
            "add",
            "--step",
            "22.5",
            "--round",
            "1",
            "--reviewer",
            "reviewer",
            "--date",
            "2026-10-17",
            "--repo",
            self.repo_dir,
        ],
        stdin=stdin,
        stderr=stderr,
    )
    self.assertNotEqual(exit_code, 0)
    self.assertIn("fields", stderr.getvalue().lower())

  def test_summary(self):
    content = (
        "date\tstep\tround\treviewer\tseverity\tcategory\t"
        "rule\tfile\tsummary\tmechanizable\toutcome\n"
        "2026-10-18\t22.5\t2\trev1\tblocker\tcorrectness\t"
        "docs/style.md 1.6\tdcfs/a.cc:1\tBad\tno\topen\n"
        "2026-10-18\t22.5\t2\trev1\tshould\ttest-gap\t"
        "AGENTS.md\tdcfs/b.cc:2\tMissing test\tcould\topen\n"
        "2026-10-18\t22.5\t2\trev1\tshould\tcorrectness\t"
        "docs/style.md 1.6\tdcfs/c.cc:3\tAnother\tcould\topen\n"
        "2026-10-17\tagents\t1\trev2\tnone\t\t\t\tclean\tno\topen\n"
    )
    with open(self.tsv_path, "w", encoding="utf-8") as f:
      f.write(content)

    stdout = io.StringIO()
    exit_code = review_findings.main(
        ["summary", "--repo", self.repo_dir], stdout=stdout
    )
    self.assertEqual(exit_code, 0)
    out = stdout.getvalue()

    # Verify Category section
    self.assertIn("Category:", out)
    self.assertIn("correctness: 2", out)
    self.assertIn("test-gap: 1", out)

    # Verify Rule section
    self.assertIn("Rule:", out)
    self.assertIn("docs/style.md 1.6: 2", out)
    self.assertIn("AGENTS.md: 1", out)

    # Verify Severity section
    self.assertIn("Severity:", out)
    self.assertIn("should: 2", out)
    self.assertIn("blocker: 1", out)
    self.assertIn("none: 1", out)

    # Verify Mechanizable section
    self.assertIn("Mechanizable:", out)
    self.assertIn("could: 2", out)
    self.assertIn("no: 2", out)


if __name__ == "__main__":
  unittest.main()
