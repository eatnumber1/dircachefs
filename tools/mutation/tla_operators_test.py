"""Tests of the TLA+ mutation operators (tla_operators.py, plan step 12.13)
on tools/mutation/testdata/snippets.tla, constructs copied from
formal/dcfs.tla, and on the real formal/dcfs.tla (sanity checks)."""

import os
import re
import sys
import unittest

import tla_operators as tla

ARGS = {}  # snippets, dcfs, arid, sets: filled from the command line


def read_text(path):
    with open(path, encoding="latin-1") as f:
        return f.read()


def snippets():
    return read_text(ARGS["snippets"])


def apply(src, m):
    return src[:m["start"]] + m["replacement"] + src[m["end"]:]


def mutants(src=None, **kw):
    src = snippets() if src is None else src
    kw.setdefault("sets", tla.parse_sets(read_text(ARGS["sets"])))
    return tla.mutants_of(src, "formal/snippets.tla", **kw)


def where(ms, function, operator=None, before=None):
    return [m for m in ms if m["function"] == function
            and (operator is None or m["operator"] == operator)
            and (before is None or " ".join(m["before"].split()) == before)]


def texts(ms, function, operator):
    """What each mutant of `function` by `operator` turns its text into."""
    return sorted(" ".join(m["replacement"].split())
                  for m in where(ms, function, operator))


class MaskTest(unittest.TestCase):

    def test_comments_and_strings_are_blanked_with_the_offsets_kept(self):
        src = 'a (* x < y (* nested *) z *) b \\* c < d\n"s<t" e\n'
        masked = tla.mask(src)
        self.assertEqual(len(masked), len(src))
        self.assertEqual(masked.count("\n"), src.count("\n"))
        self.assertNotIn("<", masked)
        self.assertNotIn("nested", masked)
        self.assertEqual(masked.split()[0], "a")
        self.assertIn("e", masked.split("\n")[1])

    def test_no_mutant_is_generated_in_a_comment(self):
        src = snippets()
        for m in mutants(src):
            line = src.count("\n", 0, m["start"])
            text = src.split("\n")[line]
            self.assertFalse(text.lstrip().startswith(("(*", "\\*")), text)
        # The banner's `x < y /\ z = 1`, the line comment's `seq <= 3`.
        for m in mutants(src):
            self.assertNotIn("Commit(a, TRUE)", m["before"])
            self.assertNotEqual(m["line"], 3)
            self.assertNotEqual(m["line"], 11)

    def test_a_mutant_changes_exactly_its_span(self):
        src = snippets()
        for m in mutants(src):
            self.assertEqual(src[m["start"]:m["end"]], m["before"])
            self.assertNotEqual(m["before"], m["replacement"])


class ScanTest(unittest.TestCase):

    def test_the_effects_of_an_extended_module_count(self):
        base = "Step == x' = 1\n"
        src = "Act == /\\ Step /\\ y > 0\n       /\\ z = 1\n"
        alone = [m for m in tla.mutants_of(src, "a.tla")
                 if m["operator"].startswith("drop")]
        self.assertIn("Step", [m["before"] for m in alone])
        both = [m for m in tla.mutants_of(src, "a.tla", extended=[base])
                if m["operator"].startswith("drop")]
        self.assertNotIn("Step", [m["before"] for m in both])
        self.assertEqual(tla.extends_of(
            "EXTENDS dcfs, Naturals\n\nCONSTANT A\n"), ["dcfs", "Naturals"])

    def test_definitions_are_found_with_their_names(self):
        names = {m["function"] for m in mutants()}
        for name in ("Commit", "CanFill", "RNCommit", "BeginMutation",
                     "Next", "Safe"):
            self.assertIn(name, names)
        # Nothing outside a definition: EXTENDS, the header.
        self.assertNotIn(None, names)

    def test_effectful_definitions_are_found_transitively(self):
        src = snippets()
        eff = tla.effectful_definitions(tla.Module(src, "x.tla"))
        self.assertIn("Commit", eff)       # primes
        self.assertIn("UnchangedBacking", eff)  # UNCHANGED
        self.assertIn("RNCommit", eff)     # through Commit
        self.assertNotIn("CanFill", eff)
        self.assertNotIn("Owns", eff)


class NegateTest(unittest.TestCase):

    def test_a_pure_conjunct_of_a_list_is_negated(self):
        ms = mutants()
        got = texts(ms, "RNProbe", "negate")
        self.assertIn('/\\ ~(At(p, "RN_probe"))', [
            " ".join(apply(snippets(), m).split("RNProbe(p) ==")[1]
                     .split("\n")[1:2]).strip() for m in ms
            if m["function"] == "RNProbe" and m["operator"] == "negate"])
        # The conjuncts that assign a primed variable or carry the frame are
        # never negated (TLC cannot evaluate them).
        self.assertFalse([t for t in got if "UnchangedBacking" in t])
        self.assertFalse([t for t in got if "AfterSyscall" in t])

    def test_an_if_condition_is_negated(self):
        ms = where(mutants(), "Commit", "negate", "sync")
        self.assertEqual([m["op"] for m in ms], ["negate-if"])
        self.assertEqual(ms[0]["replacement"], "~(sync)")

    def test_a_multiline_conjunct_keeps_its_parentheses_balanced(self):
        src = snippets()
        for m in mutants(src):
            new = apply(src, m)
            for open_, close in ("()", "[]", "{}"):
                if m["operator"] == "negate":
                    self.assertEqual(new.count(open_), src.count(open_)
                                     + m["replacement"].count(open_)
                                     - m["before"].count(open_))

    def test_inline_pieces_of_a_chain_are_negated_too(self):
        # CanFill == a \/ (b /\ c): the pieces of the parenthesis.
        got = texts(mutants(), "CanFill", "negate")
        self.assertIn("~(inflight = 0)", got)
        self.assertIn("~(seq <= s)", got)
        self.assertIn("~(BugUnguardedFills)", got)


class DropTest(unittest.TestCase):

    def test_a_guard_of_an_action_is_dropped_for_true(self):
        ms = where(mutants(), "RNProbe", "drop-guard")
        self.assertEqual([m["replacement"] for m in ms], ["TRUE"])
        self.assertEqual(" ".join(ms[0]["before"].split()),
                         'At(p, "RN_probe")')

    def test_the_effects_and_the_frame_are_never_dropped(self):
        for m in mutants():
            if m["op"].startswith(("drop-guard", "drop-conjunct")):
                self.assertNotIn("'", m["before"])
                self.assertNotIn("Unchanged", m["before"])
                self.assertNotIn("UNCHANGED", m["before"])
                self.assertNotIn("Commit", m["before"])

    def test_a_conjunct_of_a_predicate_is_dropped(self):
        ms = where(mutants(), "Safe", "drop-conjunct")
        self.assertEqual(len(ms), 4)
        self.assertTrue(all(m["replacement"] == "TRUE" for m in ms))

    def test_inline_conjuncts_are_dropped(self):
        ms = where(mutants(), "At", "drop-conjunct")
        self.assertEqual(sorted(" ".join(m["before"].split()) for m in ms),
                         ['Free(p)', 'mode = "up"', 'ps[p].pc = label'])

    def test_a_disjunct_of_the_next_state_relation_is_dropped(self):
        ms = where(mutants(), "Next", "drop-disjunct")
        before = sorted(" ".join(m["before"].split()) for m in ms)
        for piece in ("Arrive(p)", "ResolveProbe(p)", "FRead", "Recover",
                      "AtimeExpiry"):
            self.assertIn(piece, before)
        self.assertTrue(all(m["replacement"] == "FALSE" for m in ms))
        # The whole nested list `\/ Arrive(p) \/ ...` goes as one, too.
        self.assertTrue(any("SyncClearDirty" in b and "Arrive" in b
                            for b in before))

    def test_a_list_of_one_conjunct_has_nothing_to_drop(self):
        src = "Z ==\n    /\\ a < b\n"
        ms = tla.mutants_of(src, "z.tla")
        self.assertFalse([m for m in ms if m["op"].startswith("drop")])


class JunctionTest(unittest.TestCase):

    def test_a_bulleted_list_swaps_all_its_bullets(self):
        ms = where(mutants(), "Safe", "swap-junction")
        self.assertEqual(len(ms), 1)
        new = apply(snippets(), ms[0])
        body = new.split("Safe ==")[1].split("Pair")[0]
        self.assertEqual(body.count("\\/"), 4)
        self.assertNotIn("/\\", body.replace("\\/", ""))

    def test_a_list_with_an_effect_is_not_swapped(self):
        self.assertFalse(where(mutants(), "RNProbe", "swap-junction"))
        # BeginMutation's list has effects; only the pure `(durableD \/
        # sync)` inside one of them swaps.
        ms = where(mutants(), "BeginMutation", "swap-junction")
        self.assertEqual([m["before"] for m in ms], ["durableD \\/ sync"])

    def test_an_inline_chain_swaps_all_its_operators_at_once(self):
        # `a /\ b /\ c` -> `a \/ b \/ c`, never `a \/ b /\ c` (SANY:
        # no precedence between them).
        src = "Z == a /\\ b /\\ c\nY == a \\/ b\n"
        ms = [m for m in tla.mutants_of(src, "z.tla")
              if m["operator"] == "swap-junction"]
        self.assertEqual(sorted(apply(src, m).split("\n")[0] if
                                m["function"] == "Z" else
                                apply(src, m).split("\n")[1] for m in ms),
                         ["Y == a /\\ b", "Z == a \\/ b \\/ c"])
        ms = where(mutants(), "Owns", "swap-junction")
        self.assertEqual([m["replacement"] for m in ms],
                         ["inflight = 1 \\/ seq = r.mseq"])
        # CanFill == a \/ (b /\ c): two chains, the inner one in parentheses.
        ms = where(mutants(), "CanFill", "swap-junction")
        self.assertEqual(sorted(m["replacement"] for m in ms),
                         ["BugUnguardedFills /\\ (inflight = 0 /\\ seq <= s)",
                          "inflight = 0 \\/ seq <= s"])

    def test_a_disjunction_of_the_next_state_relation_is_left_alone(self):
        # An effectful disjunction (a step of the model) is never swapped.
        self.assertFalse(where(mutants(), "Bump", "swap-junction"))


class RelationalTest(unittest.TestCase):

    def test_orderings_and_equality_are_swapped(self):
        ms = mutants()
        got = {(" ".join(m["before"].split()), m["replacement"])
               for m in ms if m["operator"] == "relational"}
        self.assertIn(("<=", "<"), got)
        self.assertIn(("=", "#"), got)

    def test_an_assignment_and_an_except_path_are_not_relational(self):
        for m in where(mutants(), "Commit", "relational"):
            self.fail(m)
        for fn in ("RNCommit", "RNProbe", "BeginMutation", "Bump"):
            for m in where(mutants(), fn, "relational"):
                # Only comparisons such as `o = NoObj`, `seq = ...`.
                self.assertNotIn("'", m["before"])
        rn = [m for m in where(mutants(), "RNCommit", "relational")]
        self.assertEqual(len(rn), 1)  # `o = NoObj`; not `!.dent[n] =`
        self.assertEqual(snippets()[rn[0]["start"] - 2:rn[0]["start"]], "o ")

    def test_the_definition_arrow_and_tuples_are_not_comparisons(self):
        for m in mutants():
            if m["operator"] == "relational":
                self.assertNotIn(m["before"], ("==", "<<", ">>", "|->"))


class ConstantTest(unittest.TestCase):

    def test_a_number_goes_to_its_neighbours(self):
        got = sorted(m["replacement"]
                     for m in where(mutants(), "Count", "constant"))
        # `Count(i) == i + 1 + 2`: 1 -> 2, 0 and 2 -> 3, 1.
        self.assertEqual(got, ["0", "1", "2", "3"])

    def test_a_range_bound_is_not_nudged(self):
        self.assertFalse(where(mutants(), "Ranges", "constant"))

    def test_booleans_flip(self):
        ms = where(mutants(), "Safe", "constant", "FALSE")
        self.assertEqual([m["replacement"] for m in ms], ["TRUE"])

    def test_a_member_of_a_small_set_becomes_its_neighbour(self):
        ms = where(mutants(), "Safe", "constant", '"seq"')
        self.assertEqual(sorted(m["replacement"] for m in ms),
                         ['"ext4"', '"metaprefix"'])
        ms = where(mutants(), "RNCommit", "constant", "Absent")
        self.assertEqual(sorted(m["replacement"] for m in ms),
                         ["NoRow", "Unknown"])

    def test_the_sets_come_from_a_file_with_one_per_line(self):
        sets = tla.parse_sets("# c\nregime: \"a\" \"b\"\nx: p q r\n")
        self.assertEqual(sets, [['"a"', '"b"'], ["p", "q", "r"]])
        with self.assertRaises(tla.TlaError):
            tla.parse_sets("bad line\n")


class DurabilityTest(unittest.TestCase):

    def test_a_commit_kind_is_replaced(self):
        ms = where(mutants(), "RNCommit", "durability")
        self.assertEqual([(m["before"], m["replacement"]) for m in ms],
                         [("FALSE", "TRUE")])
        ms = where(mutants(), "BeginMutation", "durability")
        self.assertEqual([(m["before"], m["replacement"]) for m in ms],
                         [("sync", "FALSE")])

    def test_the_definition_of_commit_is_not_a_call(self):
        self.assertFalse(where(mutants(), "Commit", "durability"))


class StepTest(unittest.TestCase):

    def test_a_pc_label_skips_the_step_it_names(self):
        src = ("A(p) ==\n    /\\ At(p, \"X\")\n"
               "    /\\ ps' = [ps EXCEPT !.pc = \"Y\"]\n"
               "B(p) ==\n    /\\ At(p, \"W\")\n"
               "    /\\ ps' = [ps EXCEPT !.pc = \"X\"]\n")
        ms = [m for m in mutants(src) if m["operator"] == "drop-step"]
        self.assertEqual([(m["function"], m["replacement"]) for m in ms],
                         [("B", '"Y"')])

    def test_a_step_with_no_single_successor_is_not_skipped(self):
        # RNCommit moves to r.cont, not to a label: nothing to skip to.
        got = [m for m in mutants() if m["operator"] == "drop-step"
               and m["function"] == "RNProbe"]
        self.assertEqual(got, [])

    def test_an_element_of_a_tuple_is_dropped_or_swapped(self):
        src = snippets()
        news = sorted(
            " ".join(apply(src, m).split("Pair ==")[1].split("\n")[0].split())
            for m in where(mutants(), "Pair", "drop-step"))
        self.assertEqual(news, ["<<bCur>>", "<<bSeq>>"])
        sw = where(mutants(), "Pair", "swap-step")
        self.assertEqual(len(sw), 1)
        self.assertIn("<<bSeq, bCur>>", apply(src, sw[0]))

    def test_unchanged_tuples_are_left_alone(self):
        for fn in ("Frame", "UnchangedBacking"):
            self.assertFalse(where(mutants(), fn, "drop-step"))
            self.assertFalse(where(mutants(), fn, "swap-step"))


class AridTest(unittest.TestCase):

    def test_a_block_between_markers_has_no_mutants(self):
        src = ("A ==\n    x < y\n"
               "\\* mutation: arid begin the log is not behavior\n"
               "B ==\n    x < y\n"
               "\\* mutation: arid end\n"
               "C ==\n    x < y\n")
        got = {m["function"] for m in tla.mutants_of(src, "a.tla")}
        self.assertEqual(got, {"A", "C"})

    def test_a_trailing_marker_covers_its_line(self):
        src = ("A ==\n    /\\ x < y \\* mutation: arid a counter\n"
               "    /\\ p = q\n")
        ms = tla.mutants_of(src, "a.tla")
        self.assertTrue(ms)
        self.assertFalse([m for m in ms if m["line"] == 2])

    def test_a_marker_without_a_reason_or_unbalanced_is_an_error(self):
        for src in ("\\* mutation: arid begin\nA == 1\n"
                    "\\* mutation: arid end\n",
                    "\\* mutation: arid begin why\nA == 1\n",
                    "\\* mutation: arid end\n",
                    "A == x < y \\* mutation: arid\n"):
            with self.assertRaises(tla.TlaError, msg=src):
                tla.mutants_of(src, "a.tla")

    def test_a_view_definition_is_arid(self):
        src = "View == <<a, b>>\nOther == x < y\n"
        got = {m["function"] for m in tla.mutants_of(
            src, "a.tla", view_names={"View"})}
        self.assertEqual(got, {"Other"})

    def test_view_names_come_from_the_configurations(self):
        names = tla.view_names_in("SPECIFICATION Spec\nVIEW View\n"
                                  "\\* VIEW Not\nINVARIANTS A\n")
        self.assertEqual(names, {"View"})

    def test_rules_need_a_reason_and_match_definitions_by_regex(self):
        arid = tla.TlaArid.parse(
            "# c\ndef-regex ^Crash | state-space views\n")
        self.assertTrue(arid.def_reason("CrashImage"))
        self.assertFalse(arid.def_reason("Other"))
        with self.assertRaises(tla.TlaError):
            tla.TlaArid.parse("def-regex ^Crash\n")
        with self.assertRaises(tla.TlaError):
            tla.TlaArid.parse("nonsense x | y\n")
        src = "CrashImage == x < y\nOther == x < y\n"
        got = {m["function"] for m in tla.mutants_of(
            src, "a.tla", arid=arid)}
        self.assertEqual(got, {"Other"})

    def test_the_real_rules_have_reasons(self):
        with open(ARGS["arid"], encoding="utf-8") as f:
            tla.TlaArid.parse(f.read())


class HunksTest(unittest.TestCase):

    def test_only_the_definitions_a_hunk_touches_are_mutated(self):
        src = snippets()
        line = src.split("\n").index(
            "Owns(r) == inflight = 1 /\\ seq = r.mseq") + 1
        ms = tla.mutants_of(src, "s.tla", hunks=[(line, line)])
        self.assertEqual({m["function"] for m in ms}, {"Owns"})
        self.assertEqual(tla.mutants_of(src, "s.tla", hunks=[(1, 1)]), [])


class RealModuleTest(unittest.TestCase):
    """formal/dcfs.tla itself: every operator finds sites, none in a comment
    or a bracket-breaking mutant."""

    @classmethod
    def setUpClass(cls):
        cls.src = read_text(ARGS["dcfs"])
        cls.ms = tla.mutants_of(cls.src, "formal/dcfs.tla")

    def test_every_operator_has_sites_and_the_count_is_plausible(self):
        ops = {m["operator"] for m in self.ms}
        # (No tuple literal outside UNCHANGED: swap-step has no site here.)
        for op in ("negate", "drop-guard", "drop-conjunct", "drop-disjunct",
                   "swap-junction", "relational", "constant", "durability",
                   "drop-step"):
            self.assertIn(op, ops)
        self.assertGreater(len(self.ms), 800)

    def test_every_mutant_is_inside_a_definition_and_not_a_comment(self):
        masked = tla.mask(self.src)
        for m in self.ms:
            self.assertTrue(m["function"])
            self.assertEqual(self.src[m["start"]:m["end"]], m["before"])
            self.assertTrue(masked[m["start"]:m["end"]].strip())

    def test_brackets_stay_balanced(self):
        masked = tla.mask(self.src)
        for m in self.ms:
            before = masked[m["start"]:m["end"]]
            after = tla.mask(m["replacement"])
            for a, b in ("()", "[]", "{}"):
                self.assertEqual(before.count(a) - before.count(b),
                                 after.count(a) - after.count(b), m)

    def test_the_known_survivors_are_in_the_guards_of_commit_calls(self):
        ms = [m for m in self.ms if m["operator"] == "durability"
              and m["function"] == "S2"]
        self.assertEqual([(m["before"], m["replacement"]) for m in ms],
                         [("FALSE", "TRUE")])

    def test_ids_are_stable_across_a_move(self):
        shifted = "\n\n" + self.src
        a = {m["key"] for m in self.ms}
        b = {m["key"] for m in tla.mutants_of(shifted, "formal/dcfs.tla")}
        self.assertEqual(a, b)


if __name__ == "__main__":
    args = []
    for a in sys.argv[1:]:
        k, _, v = a.partition("=")
        if k in ("snippets", "dcfs", "arid", "sets"):
            ARGS[k] = v
        else:
            args.append(a)
    unittest.main(argv=[sys.argv[0]] + args)
