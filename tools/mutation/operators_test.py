"""Tests of the mutation operators and the arid nodes on a C++ fixture.

The fixture (fixture.cc) is compiled by the pinned clang with the command
line mutate.py builds for dcfs/ (`ast_dump_command`), so the AST under test
is clang's own; the test is hermetic (the compiler is a Bazel data
dependency, the fixture includes no header).
"""

import os
import subprocess
import sys
import unittest

import arid
import mutate
import operators

ARGS = {}  # clang_files, fixture, arid: filled from the command line


def find_clang():
    for path in ARGS["clang_files"]:
        if path.endswith("/bin/clang"):
            return path
    raise RuntimeError("no bin/clang in %r" % ARGS["clang_files"])


class Fixture(unittest.TestCase):
    """Base: the fixture and its AST, built once."""

    @classmethod
    def setUpClass(cls):
        if "loaded" in ARGS:
            cls.source, cls.functions, cls.arid = ARGS["loaded"]
            return
        with open(ARGS["fixture"], "rb") as f:
            cls.source = f.read().decode("latin-1")
        base = [find_clang(), "--no-default-config", "-nostdinc",
                "-std=c++20"]
        p = subprocess.run(
            mutate.ast_dump_command(base, ARGS["fixture"]),
            capture_output=True, check=False)
        if p.returncode != 0:
            raise RuntimeError(p.stderr.decode())
        functions = mutate.parse_stream(p.stdout.decode(), ARGS["fixture"])
        cls.functions = {fn["name"]: fn for fn in functions}
        cls.arid = arid.Arid.load(ARGS["arid"])
        ARGS["loaded"] = (cls.source, cls.functions, cls.arid)

    def sites(self, name, operator=None, filter_arid=False):
        fn = self.functions[name]
        found = operators.sites_in(fn, self.source)
        if filter_arid:
            if self.arid.function_reason(name):
                return []
            spans = self.arid.spans(fn, self.source)
            found = [s for s in found if not arid.overlaps(s, spans)]
        return [s for s in found if operator in (None, s.operator)]

    def pairs(self, name, operator=None, filter_arid=False):
        return [(s.before, s.replacement)
                for s in self.sites(name, operator, filter_arid)]


class SitesTest(Fixture):

    def test_mutated_text_is_what_the_site_says(self):
        for name in self.functions:
            for s in self.sites(name):
                self.assertEqual(self.source[s.start:s.end], s.before, name)


class RelationalTest(Fixture):

    def test_less_than_becomes_less_or_equal(self):
        self.assertEqual(self.pairs("Relational"), [("<", "<=")])

    def test_the_site_is_the_operator_token(self):
        (site,) = self.sites("Relational")
        mutated = (self.source[:site.start] + site.replacement
                   + self.source[site.end:])
        self.assertIn("return a <= b;", mutated)

    def test_every_pair(self):
        r = operators.RelationalReplacement.REPLACEMENTS
        self.assertEqual(r, {"<": "<=", "<=": "<", ">": ">=", ">=": ">",
                             "==": "!=", "!=": "=="})


class LogicalTest(Fixture):

    def test_and_becomes_or(self):
        self.assertEqual(self.pairs("Logical"), [("&&", "||")])
        self.assertEqual(operators.LogicalReplacement.REPLACEMENTS["||"],
                         "&&")


class NegationTest(Fixture):

    def test_if_while_for_and_conditional_conditions_are_negated(self):
        self.assertEqual(
            [(s.op, s.replacement) for s in self.sites("Negations", "negate")],
            [("negate-if", "!(a)"), ("negate-while", "!(a)"),
             ("negate-for", "!(i < 3)"), ("negate-?:", "!(a)")])


class EnumSwapTest(Fixture):

    def test_found_becomes_negative(self):
        self.assertEqual(self.pairs("Swaps"),
                         [("Kind::kFound", "Kind::kNegative")])


class ConstantTest(Fixture):

    def test_a_literal_in_arithmetic_is_nudged(self):
        self.assertEqual(self.pairs("Constants", "constant"),
                         [("7", "8"), ("7", "6"), ("7", "0")])

    def test_a_literal_in_a_comparison_is_nudged(self):
        self.assertEqual(self.pairs("Negations", "constant"),
                         [("3", "4"), ("3", "2"), ("3", "0")])

    def test_a_literal_outside_conditions_and_arithmetic_is_not(self):
        self.assertEqual(self.pairs("ConstantsOutsideArithmetic"), [])

    def test_one_becomes_two_and_zero(self):
        # N - 1 and 0 coincide for 1: no duplicate.
        self.assertEqual(self.pairs("Other", "constant")[3:],
                         [("1", "2"), ("1", "0")])


class StatementDeletionTest(Fixture):

    def test_calls_assignments_and_increments_are_deleted(self):
        self.assertEqual(
            [(s.op, s.before) for s in self.sites("Statements")],
            [("delete-call End", "m.End()"),
             ("delete-statement", "a = 5"),
             ("delete-statement", "++a"),
             ("delete-call Touch", "Touch(a)"),
             ("delete-call Mark", "(void)m.Mark(a)")])
        self.assertEqual({s.replacement for s in self.sites("Statements")},
                         {"(void)0"})

    def test_a_void_call_without_braces_is_deleted(self):
        self.assertEqual(self.pairs("Unbraced", "delete-statement"),
                         [("m.End()", "(void)0")])

    def test_a_status_phase_call_used_as_a_value_becomes_ok(self):
        self.assertEqual(
            self.pairs("Marks", "delete-statement"),
            [("m.Mark(1)", "absl::OkStatus()"),
             ("BeginThing()", "absl::OkStatus()")])


class StatusReturnTest(Fixture):

    def test_an_error_return_becomes_ok(self):
        self.assertEqual(self.pairs("ErrorOut", "status-return"),
                         [('absl::InternalError("boom")', "absl::OkStatus()")])
        self.assertEqual(self.pairs("Marks", "status-return"),
                         [("status", "absl::OkStatus()")])

    def test_an_ok_return_becomes_an_error(self):
        self.assertEqual(
            self.pairs("OkOut", "status-return"),
            [("absl::OkStatus()", 'absl::InternalError("mutant")')])

    def test_a_statusor_value_becomes_an_error(self):
        self.assertEqual(
            self.pairs("ValueOut", "status-return"),
            [("a", 'absl::InternalError("mutant")')])

    def test_a_returned_status_of_a_statusor_function_is_left_alone(self):
        self.assertEqual(self.pairs("StatusOut", "status-return"), [])

    def test_a_lambda_return_is_not_the_functions(self):
        self.assertEqual(self.pairs("Inner", "status-return"), [])

    def test_other_functions_are_left_alone(self):
        self.assertEqual(self.pairs("Relational", "status-return"), [])


class ArgumentSwapTest(Fixture):

    def test_two_same_typed_arguments_swap(self):
        self.assertEqual(self.pairs("Swapping", "swap-args"),
                         [("x, y", "y, x")])

    def test_identical_arguments_and_different_types_do_not(self):
        # TwoSame(x, x) and TwoSame(x, z) (an int and a long) are skipped.
        self.assertEqual(len(self.sites("Swapping", "swap-args")), 1)


class AridTest(Fixture):

    def test_a_mutant_in_a_log_stream_is_not_generated(self):
        kept = self.pairs("Logging", filter_arid=True)
        self.assertEqual(kept, [("==", "!=")])  # the CHECK's condition only
        self.assertGreater(len(self.pairs("Logging")), len(kept))

    def test_a_check_condition_is_mutated_but_not_its_message(self):
        before = [b for b, _ in self.pairs("Logging")]
        after = [b for b, _ in self.pairs("Logging", filter_arid=True)]
        self.assertIn("!=", before)
        self.assertNotIn("!=", after)
        self.assertIn("==", after)

    def test_diagnostic_functions_have_no_mutants(self):
        for name in ("ToString", "DebugString", "AbslStringify"):
            self.assertTrue(self.pairs(name), name)
            self.assertEqual(self.pairs(name, filter_arid=True), [], name)
        self.assertTrue(self.pairs("Other", filter_arid=True))

    def test_an_error_builder_message_is_arid(self):
        self.assertEqual(self.pairs("Messages", filter_arid=True), [])
        self.assertTrue(self.pairs("Messages"))

    def test_collect_mutants_applies_the_arid_rules(self):
        fns = list(self.functions.values())
        rules = [("all", [])]
        plain = mutate.collect_mutants(fns, self.source, "f.cc", rules)
        pruned = mutate.collect_mutants(fns, self.source, "f.cc", rules,
                                        self.arid)
        self.assertLess(len(pruned), len(plain))
        self.assertFalse([m for m in pruned if m["function"] in (
            "ToString", "DebugString", "AbslStringify")])
        self.assertFalse([m for m in pruned if m["function"] == "Messages"])

    def test_every_rule_has_a_reason(self):
        with self.assertRaises(arid.AridError):
            arid.Arid.parse("macro LOG\n")
        with self.assertRaises(arid.AridError):
            arid.Arid.parse("macro LOG | \n")
        with self.assertRaises(arid.AridError):
            arid.Arid.parse("nonsense LOG | because\n")
        with open(ARGS["arid"], encoding="utf-8") as f:
            arid.Arid.parse(f.read())

    def test_regex_rules_may_hold_a_bar(self):
        a = arid.Arid.parse("function-regex ^A$|^B | why\n")
        self.assertEqual(a.function_reason("B"), "why")
        self.assertIsNone(a.function_reason("C"))


if __name__ == "__main__":
    # `$(rootpaths ...)` expands to several arguments: the first is
    # `clang_files=PATH`, the rest are bare paths of the same filegroup.
    rest = []
    ARGS["clang_files"] = []
    for arg in sys.argv[1:]:
        key, eq, value = arg.partition("=")
        if eq and key == "clang_files":
            ARGS["clang_files"].append(value)
        elif eq and key in ("fixture", "arid"):
            ARGS[key] = value
        elif "/" in arg and not arg.startswith("-"):
            ARGS["clang_files"].append(arg)
        else:
            rest.append(arg)
    unittest.main(argv=[sys.argv[0]] + rest)
