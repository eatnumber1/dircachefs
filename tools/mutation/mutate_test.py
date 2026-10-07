"""Tests of the mutation tester's operators over small synthetic AST nodes."""

import unittest

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


if __name__ == "__main__":
    unittest.main()
