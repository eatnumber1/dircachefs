"""Self-check of tools/style_checks.py (plan step 25.21).

Canned clang-tidy and clang-query outputs and fixture sources: the parsers,
the function a finding is keyed by, the allowlist comparison (new findings
and stale lines both fail), the deny-list reasons and the fixture rules must
each report what they are for and nothing on a good input.
"""

import os
import tempfile
import unittest

import style_checks

TIDY = """\
dcfs/a.cc:12:3: error: do not use 'else' after 'return' [readability-else-after-return,-warnings-as-errors]
   12 |   } else {
      |     ^~~~~~
./dcfs/a.h:5:1: error: two checks [misc-a,misc-b,-warnings-as-errors]
external/abseil-cpp+/absl/x.h:1:1: error: not ours [misc-a,-warnings-as-errors]
"""

QUERY = """\
Match #1:

./dcfs/a.cc:10:1:{10:1-20:2}: note: "function" binds here
   10 |  absl::Status Backing::Open(const Path &p) {
      |  ^~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Match #2:

/work/execroot/_main/dcfs/a.cc:14:5:{14:5-14:30}: note: "banned_std" binds here
   14 |    std::function<void()> f;
      |    ^~~~~~~~~~~~~~~~~~~~~~~

Match #3:

./dcfs/a.cc:15:5:{15:5-15:9}: note: "captured" binds here
   15 |    int count;
      |    ^~~~~~~~
"""


class ParsingTest(unittest.TestCase):

    def test_tidy_findings_are_split_per_check_and_drop_the_error_marker(self):
        findings, failures = style_checks.parse_tidy(TIDY)
        self.assertEqual([], failures)
        self.assertEqual(
            [("dcfs/a.cc", 12, "readability-else-after-return"),
             ("dcfs/a.h", 5, "misc-a"), ("dcfs/a.h", 5, "misc-b"),
             ("external/abseil-cpp+/absl/x.h", 1, "misc-a")],
            [(f.file, f.line, f.rule) for f in findings])

    def test_an_unparsable_file_is_a_failure(self):
        _, failures = style_checks.parse_tidy(
            "Error while processing dcfs/a.cc.\n"
            "dcfs/a.cc:1:10: fatal error: 'x.h' file not found\n")
        self.assertEqual(2, len(failures))

    def test_query_findings_and_functions(self):
        # A name bound inside a matcher ("captured") is not a rule.
        findings, functions, failures = style_checks.parse_query(QUERY)
        self.assertEqual([], failures)
        self.assertEqual([("dcfs/a.cc", 10, 20, "Backing::Open")], functions)
        self.assertEqual([("dcfs/a.cc", 14, "banned_std")],
                         [(f.file, f.line, f.rule) for f in findings])

    def test_a_query_that_could_not_parse_the_file_is_a_failure(self):
        _, _, failures = style_checks.parse_query(
            "dcfs/a.cc:1:10: fatal error: 'x.h' file not found\n")
        self.assertEqual(1, len(failures))

    def test_function_name_is_the_identifier_before_the_first_parenthesis(self):
        name = style_checks.function_name
        self.assertEqual("Backing::Open", name("absl::Status Backing::Open("))
        self.assertEqual("operator==", name("bool operator==(const A &a) {"))
        self.assertEqual("Foo::~Foo", name("Foo::~Foo() {"))
        self.assertEqual("template <class T>", name("template <class T>"))

    def test_the_innermost_function_holding_a_line_names_a_finding(self):
        functions = {("dcfs/a.cc", 1, 40, "Outer"),
                     ("dcfs/a.cc", 10, 20, "Outer::Inner")}
        enclosing = style_checks.enclosing_function
        self.assertEqual("Outer::Inner",
                         enclosing(functions, "dcfs/a.cc", 15))
        self.assertEqual("Outer", enclosing(functions, "dcfs/a.cc", 30))
        self.assertEqual(style_checks.FILE_SCOPE,
                         enclosing(functions, "dcfs/a.cc", 50))
        self.assertEqual(style_checks.FILE_SCOPE,
                         enclosing(functions, "dcfs/b.cc", 15))


class ReadOutputsTest(unittest.TestCase):

    def test_outputs_are_read_deduplicated_and_clang_diagnostics_fail(self):
        with tempfile.TemporaryDirectory() as root:
            paths = []
            for name, text in (
                    ("a.cc.tidy.txt",
                     "dcfs/a.h:5:1: error: x [misc-a,-warnings-as-errors]\n"
                     "dcfs/a.h:9:1: error: y [clang-diagnostic-unused-variable]\n"),
                    ("b.cc.tidy.txt",
                     "dcfs/a.h:5:1: error: x [misc-a,-warnings-as-errors]\n"),
                    ("a.cc.query.txt", QUERY)):
                path = os.path.join(root, name)
                with open(path, "w", encoding="utf-8") as f:
                    f.write(text)
                paths.append(path)
            findings, functions, failures = style_checks.read_outputs(paths)
        self.assertEqual(
            [("dcfs/a.cc", 14, "banned_std"), ("dcfs/a.h", 5, "misc-a")],
            [(f.file, f.line, f.rule) for f in findings])
        self.assertEqual({("dcfs/a.cc", 10, 20, "Backing::Open")}, functions)
        self.assertEqual(1, len(failures))
        self.assertIn("dcfs/a.h:9: clang-diagnostic-unused-variable", failures[0])


class AllowlistTest(unittest.TestCase):

    KEY = ("dcfs/a.cc", "Backing::Open", "banned_std")

    def test_a_new_finding_and_a_stale_line_both_fail(self):
        allowed = style_checks.parse_allowlist(
            "# comment\n\ndcfs/a.cc|Backing::Open|banned_std|2\n"
            "dcfs/b.cc|operator||misc-a|1\n")
        self.assertEqual(2, allowed[self.KEY])
        self.assertEqual(1, allowed[("dcfs/b.cc", "operator|", "misc-a")])
        self.assertEqual([], style_checks.compare({self.KEY: 2}, {self.KEY: 2}))
        new = style_checks.compare({self.KEY: 3}, {self.KEY: 2})
        self.assertEqual(1, len(new))
        self.assertTrue(new[0].startswith("new: dcfs/a.cc|Backing::Open|"))
        stale = style_checks.compare({self.KEY: 1}, {self.KEY: 2})
        self.assertEqual(1, len(stale))
        self.assertTrue(stale[0].startswith("stale: "))
        self.assertEqual(1, len(style_checks.compare({}, {self.KEY: 2})))
        self.assertEqual(1, len(style_checks.compare({self.KEY: 1}, {})))

    def test_report_only_rules_never_fail(self):
        report_key = ("dcfs/a.cc", "F", "status_uninitialized")
        enforced, report = style_checks.split_report_only(
            {self.KEY: 1, report_key: 4})
        self.assertEqual({self.KEY: 1}, enforced)
        self.assertEqual({report_key: 4}, report)


class ConfigTest(unittest.TestCase):

    def test_a_denied_check_needs_its_reason(self):
        good = ("# Denied:\n#   misc-a: because.\nChecks: >\n  -*,\n  misc-*,\n"
                "  -misc-a\nWarningsAsErrors: '*'\n")
        self.assertEqual([], style_checks.tidy_denials(good))
        bad = good.replace("#   misc-a: because.\n", "")
        self.assertEqual(
            [".clang-tidy denies misc-a with no `#   misc-a: reason` line"],
            style_checks.tidy_denials(bad))
        self.assertEqual([".clang-tidy has no Checks"],
                         style_checks.tidy_denials("WarningsAsErrors: '*'\n"))
        quoted = "Checks: '-*,misc-*,-misc-a'\nWarningsAsErrors: '*'\n"
        self.assertEqual(1, len(style_checks.tidy_denials(quoted)))


class FixtureTest(unittest.TestCase):

    BAD = "int f() {\n  return 1;  // HIT\n}\n"

    def finding(self, path, line, rule):
        return style_checks.Finding(path, line, 1, rule, "")

    def problems(self, findings, sources):
        return style_checks.fixture_problems(findings, sources)

    def test_a_bad_fixture_must_have_exactly_its_marked_reports(self):
        path = "tools/style_matchers/banned_std_bad.cc"
        hit = self.finding(path, 2, "banned_std")
        self.assertEqual([], self.problems([hit], {path: self.BAD}))
        self.assertEqual(1, len(self.problems([], {path: self.BAD})))
        extra = self.finding(path, 1, "banned_std")
        self.assertEqual(1, len(self.problems([hit, extra], {path: self.BAD})))
        other = self.finding(path, 1, "misc-a")
        self.assertEqual([], self.problems([hit, other], {path: self.BAD}))

    def test_a_good_fixture_must_have_no_report_of_its_rule(self):
        for name in ("banned_std_good.cc", "banned_std_exempt_test.cc",
                     "testonly/banned_std_exempt.cc"):
            path = "tools/style_matchers/" + name
            hit = self.finding(path, 1, "banned_std")
            self.assertEqual([], self.problems([], {path: "int x;\n"}))
            self.assertEqual(1, len(self.problems([hit], {path: "int x;\n"})))

    def test_the_tidy_fixtures(self):
        bad = "tools/style_matchers/tidy_bad.cc"
        good = "tools/style_matchers/tidy_good.cc"
        text = "int f() {  // HIT misc-a\n}\n"
        self.assertEqual(
            [], self.problems([self.finding(bad, 1, "misc-a")], {bad: text}))
        self.assertEqual(1, len(self.problems([], {bad: text})))
        self.assertEqual([], self.problems([], {good: "int x;\n"}))
        self.assertEqual(
            1, len(self.problems([self.finding(good, 1, "misc-b")],
                                 {good: "int x;\n"})))

    def test_a_fixture_of_no_matcher_is_a_problem(self):
        path = "tools/style_matchers/nothing_bad.cc"
        self.assertEqual(1, len(self.problems([], {path: "int x;\n"})))

    def test_matcher_files_name_their_style_section_and_are_all_there(self):
        texts = {rule: "# docs/style.md 1.2: why.\nmatch x\n"
                 for rule in style_checks.MATCHER_RULES}
        texts["00_functions"] = "set output diag\n"
        self.assertEqual([], style_checks.matcher_file_problems(texts))
        texts["banned_std"] = "# a comment\nmatch x\n"
        texts["stray"] = "# docs/style.md\nmatch x\n"
        del texts["status_uninitialized"]
        self.assertEqual(3, len(style_checks.matcher_file_problems(texts)))


if __name__ == "__main__":
    unittest.main()
