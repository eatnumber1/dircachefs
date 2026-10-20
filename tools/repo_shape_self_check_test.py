# SPDX-License-Identifier: MIT
"""Self-check of the repository-shape rules (plan steps 26.1 and 26.13).

Fixture trees with a missing README, an unreferenced script, an unlisted
DISABLED_ check and a bare sleep: each rule must report it; a good tree
reports nothing.
"""

import os
import subprocess
import sys
import tempfile
import unittest

import repo_shape


def write(root, path, text=""):
    full = os.path.join(root, path)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    with open(full, "w", encoding="utf-8") as f:
        f.write(text)


def good_tree(root):
    write(root, "third_party/pin/README.md")
    write(root, "test/qemu/BUILD.bazel",
          'qemu_test(name = "a_test", guest_script = "guest/a.sh")\n'
          'qemu_test(name = "s", guest_script = "guest/shard_%s.sh" % x)\n'
          'qemu_test(name = "l", guest_script = "guest/legacy.sh")\n')
    write(root, "test/qemu/guest/lib.sh",
          'disabled() { :; }\n'
          'justified_sleep() {\n\tsleep "$1"\n}\n'
          '# a comment about sleep 5 is not a sleep\n'
          'require_commands sleep sort\n')
    write(root, "tools/repo_shape_sleeps.txt",
          "# known\ntest/qemu/guest/legacy.sh 2 | polls a pid\n")
    write(root, "test/qemu/guest/legacy.sh", "sleep 1\n\tsleep 0.1\n")
    write(root, "tools/repo_shape_fixed_arrays.txt",
          "# known\ndcfs/known.cc 1 | a fixture reason\n")
    write(root, "dcfs/known.cc",
          "void f(int n) {\n  std::vector<int> v(n);\n}\n")
    write(root, "tools/repo_shape_absl_macros.txt", "# known\n")
    write(root, "test/qemu/guest/init")
    write(root, "tools/repo_shape_guest_shell.txt",
          "# known\ntest/qemu/guest/lib.sh\ntest/qemu/guest/legacy.sh\n"
          "test/qemu/guest/a.sh\ntest/qemu/guest/helper.sh\n"
          "test/qemu/guest/shard_one.sh\ntest/qemu/wrapper_test.sh\n")
    write(root, "test/qemu/wrapper_test.sh", "exec true\n")
    write(root, "test/qemu/guest/a.sh",
          '. "$(dirname "$0")/helper.sh"\ndisabled kernel-bug "why" check\n')
    write(root, "test/qemu/guest/helper.sh")
    write(root, "test/qemu/guest/shard_one.sh")
    write(root, "dcfs/a.h",
          "// A cache of names, bounded by max_entries.\n"
          "// Unit tests set it themselves; the suite's own tools too.\n"
          "int a;  // the tests for this are in a_test.cc\n")
    write(root, "dcfs/a_test.cc", "// only in tests, for testing.\n")
    write(root, "dcfs/testonly/b.h", "// test-only helper.\n")
    write(root, "README.md",
          "# x\n\n## Limitations\n\n- the `kernel-bug` check.\n\n## More\n")
    write(root, "docs/style.md",
          '# Style\n\n<a id="rule-no-timers"></a>\n### 1.11 No timers\n\n'
          '- <a id="rule-test-first"></a>**Test first.**\n')
    write(root, "docs/design.md", "# Design\n")
    write(root, "docs/plan/process.md", "# Process\n")
    write(root, "CLAUDE.md", "# Claude\n")
    write(root, ".claude/agents/dcfs-x.md", "---\nname: dcfs-x\n---\nA role.\n")
    write(root, "tools/repo_shape_attributions.txt",
          "# known\n.claude/agents/dcfs-known.md 1 | role-only: a fixture\n")
    write(root, ".claude/agents/dcfs-known.md",
          "Why this role exists (russ, 2026-10-16: the pre-commit review).\n")
    write(root, "docs/rules/index-style.md",
          "- No timers. [style](/docs/style.md#rule-no-timers)\n"
          "- Test first. [style](/docs/style.md#rule-test-first)\n")
    write(root, "AGENTS.md",
          "# Rules\n\n| Task | Read |\n|---|---|\n"
          "| Style | [style](docs/rules/index-style.md) |\n")


class RepoShapeSelfCheckTest(unittest.TestCase):

    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.root = self.dir.name
        good_tree(self.root)

    def tearDown(self):
        self.dir.cleanup()

    def test_a_rule_in_no_index_is_reported(self):
        write(self.root, "docs/design.md",
              '# Design\n\n<a id="rule-bounded-memory"></a>\n'
              '### Bounded memory\n')
        problems = repo_shape.rule_index(self.root)
        self.assertEqual(1, len(problems), problems)
        self.assertIn("docs/design.md marks rule bounded-memory", problems[0])
        self.assertIn("no docs/rules/index-*.md links to it", problems[0])

    def test_a_rule_in_two_indexes_is_reported(self):
        write(self.root, "docs/rules/index-code.md",
              "- Test first. [style](/docs/style.md#rule-test-first)\n")
        write(self.root, "AGENTS.md",
              "[a](docs/rules/index-style.md) [b](docs/rules/index-code.md)\n")
        problems = repo_shape.rule_index(self.root)
        self.assertEqual(1, len(problems), problems)
        self.assertIn("rule test-first of docs/style.md is in 2 indexes",
                      problems[0])

    def test_an_index_entry_for_a_rule_that_is_gone_is_reported(self):
        write(self.root, "docs/rules/index-style.md",
              "- No timers. [style](/docs/style.md#rule-no-timers)\n"
              "- Test first. [style](/docs/style.md#rule-test-first)\n"
              "- Gone. [process](/docs/plan/process.md#rule-gone)\n")
        problems = repo_shape.rule_index(self.root)
        self.assertEqual(1, len(problems), problems)
        self.assertIn("docs/rules/index-style.md links to "
                      "docs/plan/process.md#rule-gone", problems[0])

    def test_an_index_that_agents_md_does_not_link_is_reported(self):
        write(self.root, "docs/rules/index-code.md", "Nothing yet.\n")
        problems = repo_shape.rule_index(self.root)
        self.assertEqual(1, len(problems), problems)
        self.assertIn("docs/rules/index-code.md is not linked from AGENTS.md",
                      problems[0])

    def test_an_index_only_mentioned_in_agents_md_is_reported(self):
        write(self.root, "docs/rules/index-code.md", "Nothing yet.\n")
        write(self.root, "AGENTS.md",
              "[a](docs/rules/index-style.md) docs/rules/index-code.md\n")
        problems = repo_shape.rule_index(self.root)
        self.assertEqual(1, len(problems), problems)
        self.assertIn("docs/rules/index-code.md is not linked from AGENTS.md",
                      problems[0])

    def test_a_relative_index_link_is_reported(self):
        write(self.root, "docs/rules/index-style.md",
              "- No timers. [style](../style.md#rule-no-timers)\n"
              "- Test first. [style](/docs/style.md#rule-test-first)\n")
        problems = repo_shape.rule_index(self.root)
        self.assertEqual(2, len(problems), problems)
        self.assertIn("docs/rules/index-style.md links to ../style.md"
                      "#rule-no-timers with a relative path", problems[0])

    def test_a_restated_attribution_in_an_agent_file_is_reported(self):
        write(self.root, ".claude/agents/dcfs-x.md",
              "No timers (russ,\n2026-10-09: wait on the event).\n")
        problems = repo_shape.agent_file_attributions(self.root)
        self.assertEqual(1, len(problems), problems)
        self.assertIn(".claude/agents/dcfs-x.md has 1", problems[0])

    def test_a_restated_attribution_in_a_skill_is_reported(self):
        write(self.root, ".claude/skills/x/SKILL.md",
              "A rule (russ, 2026-10-06).\n")
        problems = repo_shape.agent_file_attributions(self.root)
        self.assertEqual(1, len(problems), problems)
        self.assertIn(".claude/skills/x/SKILL.md has 1", problems[0])

    def test_claude_md_attributions_are_counted_too(self):
        write(self.root, "CLAUDE.md", "A rule (russ, 2026-10-06).\n")
        problems = repo_shape.agent_file_attributions(self.root)
        self.assertEqual(1, len(problems), problems)
        self.assertIn("CLAUDE.md has 1", problems[0])

    def test_a_listed_attribution_count_must_match(self):
        write(self.root, ".claude/agents/dcfs-known.md", "No attribution.\n")
        problems = repo_shape.agent_file_attributions(self.root)
        self.assertEqual(1, len(problems), problems)
        self.assertIn("lists 1 for .claude/agents/dcfs-known.md, which has 0",
                      problems[0])

    def test_the_parenthesised_attribution_form_is_reported(self):
        write(self.root, ".claude/agents/dcfs-x.md",
              "Choose the effort (russ (2026-10-09): leverage levels).\n")
        problems = repo_shape.agent_file_attributions(self.root)
        self.assertEqual(1, len(problems), problems)

    def test_a_listed_file_that_is_gone_is_reported(self):
        os.remove(os.path.join(self.root, ".claude/agents/dcfs-known.md"))
        problems = repo_shape.agent_file_attributions(self.root)
        self.assertEqual(1, len(problems), problems)
        self.assertIn(".claude/agents/dcfs-known.md, which is gone",
                      problems[0])

    def test_a_malformed_attribution_line_is_reported_not_raised(self):
        write(self.root, "tools/repo_shape_attributions.txt",
              "# known\nCLAUDE.md lots | a bad count\njust-a-path | no count\n")
        problems = repo_shape.agent_file_attributions(self.root)
        self.assertEqual(3, len(problems), problems)
        self.assertIn("tools/repo_shape_attributions.txt: cannot read",
                      problems[0])

    def test_a_marker_used_twice_in_a_file_is_reported(self):
        write(self.root, "CLAUDE.md",
              '- <a id="rule-x"></a>One.\n- <a id="rule-x"></a>Two.\n')
        write(self.root, "docs/rules/index-style.md",
              "[a](/docs/style.md#rule-no-timers) "
              "[b](/docs/style.md#rule-test-first) [c](/CLAUDE.md#rule-x)\n")
        problems = repo_shape.rule_index(self.root)
        self.assertEqual(1, len(problems), problems)
        self.assertIn("CLAUDE.md marks rule x twice", problems[0])

    def test_new_guest_shell_is_reported(self):
        write(self.root, "test/qemu/guest/new.sh", "echo hi\n")
        problems = repo_shape.no_new_guest_shell(self.root)
        self.assertEqual(1, len(problems))
        self.assertIn("test/qemu/guest/new.sh is new shell", problems[0])

    def test_new_shell_beside_the_guests_is_reported(self):
        write(self.root, "test/qemu/scripts/new.sh", "echo hi\n")
        self.assertEqual(1, len(repo_shape.no_new_guest_shell(self.root)))

    def test_a_listed_script_that_is_gone_is_reported(self):
        os.remove(os.path.join(self.root, "test/qemu/wrapper_test.sh"))
        problems = repo_shape.no_new_guest_shell(self.root)
        self.assertEqual(1, len(problems))
        self.assertIn("wrapper_test.sh, which is gone", problems[0])

    def test_missing_readme_is_reported(self):
        os.remove(os.path.join(self.root, "third_party/pin/README.md"))
        problems = repo_shape.third_party_readmes(self.root)
        self.assertEqual(1, len(problems), problems)
        self.assertIn("third_party/pin/", problems[0])

    def test_long_status_macro_is_reported_and_comments_are_not(self):
        write(self.root, "bench/new.cc",
              "// ABSL_RETURN_IF_ERROR(x) is named in a comment only.\n"
              "absl::Status f() {\n  ABSL_RETURN_IF_ERROR(g());\n}\n")
        problems = repo_shape.absl_prefixed_status_macros(self.root)
        self.assertEqual(1, len(problems), problems)
        self.assertIn("bench/new.cc has 1", problems[0])

    def test_listed_status_macro_count_must_match(self):
        write(self.root, "tools/repo_shape_absl_macros.txt",
              "dcfs/known.cc 2 | a fixture reason\n")
        write(self.root, "dcfs/known.cc",
              "void f() {\n  ABSL_ASSIGN_OR_RETURN(int x, g());\n}\n")
        problems = repo_shape.absl_prefixed_status_macros(self.root)
        self.assertEqual(1, len(problems), problems)
        self.assertIn("dcfs/known.cc has 1", problems[0])

    def test_nodiscard_on_a_status_is_reported_and_on_a_descriptor_is_not(self):
        write(self.root, "dcfs/nd.h",
              "[[nodiscard]] absl::Status Close(int fd);\n"
              "[[nodiscard]] absl::StatusOr<int> Open();\n"
              "[[nodiscard]] FileDescriptor Own();\n")
        problems = repo_shape.nodiscard_on_status(self.root)
        self.assertEqual(2, len(problems), problems)
        self.assertIn("dcfs/nd.h:1", problems[0])
        self.assertIn("dcfs/nd.h:2", problems[1])

    def test_unreferenced_script_is_reported(self):
        write(self.root, "test/qemu/guest/orphan.sh")
        problems = repo_shape.guest_scripts_used(self.root)
        self.assertEqual(1, len(problems), problems)
        self.assertIn("orphan.sh", problems[0])

    def test_unlisted_disabled_check_is_reported(self):
        write(self.root, "test/qemu/guest/b.sh", "disabled other-bug why c\n")
        write(self.root, "test/qemu/BUILD.bazel",
              'guest_script = "guest/a.sh"\nguest_script = "guest/b.sh"\n')
        problems = repo_shape.disabled_checks_listed(self.root)
        self.assertEqual(1, len(problems), problems)
        self.assertIn("DISABLED_other-bug", problems[0])

    def test_a_new_bare_sleep_is_reported(self):
        write(self.root, "test/qemu/guest/a.sh",
              '. "$(dirname "$0")/helper.sh"\ndisabled kernel-bug "why" check\n'
              'sleep 1\n')
        problems = repo_shape.guest_sleeps(self.root)
        self.assertEqual(1, len(problems), problems)
        self.assertIn("a.sh has 1 bare sleep", problems[0])
        self.assertIn("No timers", problems[0])

    def test_more_sleeps_than_the_list_knows_are_reported(self):
        write(self.root, "test/qemu/guest/legacy.sh",
              "sleep 1\nsleep 2\nsleep \"$n\"\n")
        problems = repo_shape.guest_sleeps(self.root)
        self.assertEqual(1, len(problems), problems)
        self.assertIn("legacy.sh has 3 bare sleep(s), 2 known", problems[0])

    def test_a_removed_sleep_must_come_off_the_list(self):
        write(self.root, "test/qemu/guest/legacy.sh", "sleep 1\n")
        problems = repo_shape.guest_sleeps(self.root)
        self.assertEqual(1, len(problems), problems)
        self.assertIn("lower it", problems[0])

    def test_sleep_as_a_word_a_comment_or_the_helper_is_not_a_sleep(self):
        # lib.sh of the good tree has all three and reports nothing.
        self.assertEqual([], repo_shape.guest_sleeps(self.root))
        self.assertEqual(
            0, repo_shape.count_sleeps(
                'echo hi # sleep 2\nsleeping 5\n'
                'grep "disk sleep)" f\n'))

    def test_name_outside_the_limitations_section_does_not_count(self):
        write(self.root, "README.md",
              "# x\n\nthe kernel-bug check\n\n## Limitations\n\n- none\n")
        self.assertEqual(1, len(repo_shape.disabled_checks_listed(self.root)))

    def test_test_reasons_in_production_comments_are_reported(self):
        write(self.root, "dcfs/c.h",
              "int a;  // they nest only in tests\n"
              "/* A knob\n * for tests of the command line. */\n"
              "// test-only\n// for testing\n// ForTest\n"
              "// so that a test can reach it\n"
              "// IF NOT EXISTS: a test that makes a v2 database\n"
              "void f_for_test();\n")
        write(self.root, "bench/d.cc", "// only used by tests\n")
        problems = repo_shape.no_test_only_comments(self.root)
        self.assertEqual(9, len(problems), problems)
        self.assertIn("dcfs/c.h:1:", problems[0])
        self.assertIn("dcfs/c.h:3:", problems[1])
        self.assertTrue(any("bench/d.cc:1:" in p for p in problems))

    def test_tests_and_testonly_sources_are_not_scanned(self):
        self.assertEqual([], repo_shape.no_test_only_comments(self.root))

    def test_suite_sentences_are_not_reasons(self):
        write(self.root, "dcfs/e.h",
              "// Run for the test suite by CI.\n"
              "// The testonly builds install the recorder.\n")
        self.assertEqual([], repo_shape.no_test_only_comments(self.root))

    def test_allowlist_entry_silences_one_comment(self):
        write(self.root, "dcfs/f.h", "// only in tests\n// for testing\n")
        allow = {("dcfs/f.h", "only in tests"): "a reason"}
        problems = repo_shape.no_test_only_comments(self.root, allow)
        self.assertEqual(1, len(problems), problems)
        self.assertIn("dcfs/f.h:2:", problems[0])

    def test_allowlist_entry_needs_a_reason(self):
        with self.assertRaises(ValueError):
            repo_shape.no_test_only_comments(
                self.root, {("dcfs/f.h", "only in tests"): ""})

    def test_friend_of_a_testonly_class_is_reported(self):
        write(self.root, "dcfs/g.h",
              "class G {\n  friend struct testonly::GPeer;\n"
              "  friend class ::dcfs::testonly::Other;\n"
              "  friend class H;\n};\n"
              "// friend testonly::NotCode is only a comment.\n")
        write(self.root, "dcfs/testonly/p.h",
              "struct P { friend struct testonly::Q; };\n")
        write(self.root, "dcfs/g_test.cc",
              "struct T { friend struct testonly::Q; };\n")
        problems = repo_shape.no_testonly_friends(self.root)
        self.assertEqual(2, len(problems), problems)
        self.assertIn("dcfs/g.h:2:", problems[0])
        self.assertIn("dcfs/g.h:3:", problems[1])

    def test_good_tree_has_no_testonly_friends(self):
        self.assertEqual([], repo_shape.no_testonly_friends(self.root))

    def test_an_identifier_with_the_dcfs_prefix_is_reported(self):
        write(self.root, "dcfs/y.h",
              "namespace dcfs {\nint DcfsFoo();\n}  // namespace dcfs\n")
        problems = repo_shape.project_prefix(self.root)
        self.assertEqual(1, len(problems), problems)
        self.assertIn("dcfs/y.h:2:", problems[0])
        self.assertIn("docs/style.md, 1.3", problems[0])

    def test_a_dcfs_word_in_a_comment_or_a_string_is_not_reported(self):
        write(self.root, "dcfs/z.cc",
              "// DcfsFoo is named here in a comment\n"
              "/* and DcfsBar\n   in a block comment */\n"
              "const char *kType = \"fuse.dcfs\";\n"
              "const char *kName = \"DcfsFoo\";\n")
        self.assertEqual([], repo_shape.project_prefix(self.root))

    def test_dcfs_inside_a_longer_name_is_not_reported(self):
        write(self.root, "dcfs/aa.h",
              "bool ProducedByDcfs(int);\nint dcfs_helper;\n")
        self.assertEqual([], repo_shape.project_prefix(self.root))

    def test_a_vector_sized_once_is_reported(self):
        write(self.root, "dcfs/m.cc",
              "void f(int n) {\n  std::vector<int> v(n);\n}\n")
        problems = repo_shape.fixed_arrays(self.root)
        self.assertEqual(1, len(problems), problems)
        self.assertIn("dcfs/m.cc has 1 local buffer", problems[0])
        self.assertIn("absl::FixedArray", problems[0])

    def test_a_string_filled_once_is_reported(self):
        write(self.root, "tools/n.cc",
              "void f(int n) {\n  std::string s(n, '\\0');\n}\n")
        problems = repo_shape.fixed_arrays(self.root)
        self.assertEqual(1, len(problems), problems)
        self.assertIn("tools/n.cc has 1 local buffer", problems[0])

    def test_iterator_pairs_and_brace_inits_are_not_reported(self):
        write(self.root, "dcfs/o.cc",
              "void f(const std::vector<int>& a) {\n"
              "  std::vector<int> v(a.begin(), a.end());\n"
              "  std::vector<int> w{a.size()};\n"
              "  std::vector<int> x = a;\n"
              "  std::string s(3, 'x');\n"
              "}\n")
        self.assertEqual([], repo_shape.fixed_arrays(self.root))

    def test_a_test_or_testonly_file_is_not_scanned(self):
        write(self.root, "dcfs/p_test.cc",
              "void f(int n) {\n  std::vector<int> v(n);\n}\n")
        write(self.root, "dcfs/testonly/q.cc",
              "void f(int n) {\n  std::vector<int> v(n);\n}\n")
        self.assertEqual([], repo_shape.fixed_arrays(self.root))

    def test_an_allowlisted_count_is_accepted(self):
        write(self.root, "dcfs/known.cc",
              "void f(int n) {\n  std::vector<int> v(n);\n}\n"
              "void g(int n) {\n  std::string s(n, '\\0');\n}\n")
        write(self.root, "tools/repo_shape_fixed_arrays.txt",
              "dcfs/known.cc 2 | a fixture reason\n")
        self.assertEqual([], repo_shape.fixed_arrays(self.root))

    def test_an_allowlisted_file_with_a_wrong_count_is_reported(self):
        write(self.root, "tools/repo_shape_fixed_arrays.txt",
              "dcfs/known.cc 2 | a fixture reason\n")
        problems = repo_shape.fixed_arrays(self.root)
        self.assertEqual(1, len(problems), problems)
        self.assertIn("the list only shrinks", problems[0])
        write(self.root, "dcfs/known.cc",
              "void f(int n) {\n  std::vector<int> v(n);\n"
              "  std::vector<int> w(n);\n  std::vector<int> x(n);\n}\n")
        write(self.root, "tools/repo_shape_fixed_arrays.txt",
              "dcfs/known.cc 1 | a fixture reason\n")
        problems = repo_shape.fixed_arrays(self.root)
        self.assertEqual(2, len(problems), problems)
        self.assertIn("dcfs/known.cc has 3 local buffer(s) sized once, "
                      "1 known", problems[0])
        self.assertIn("lists 1 buffer(s) for dcfs/known.cc, which has 3",
                      problems[1])

    def spdx_problems(self, files, third_party=()):
        """Runs spdx_licenses over fixture files (the keys are the tracked
        list) with the fixture licence tables."""
        write(self.root, "tools/licenses_allowed.txt",
              "# id | reason\n"
              "MIT | ours\n"
              "BSD-3-Clause | copied permissive code, listed in "
              "third_party_files.txt\n"
              "GPL-2.0-only | test-only patches to GPL code, listed with role "
              "test-only\n")
        write(self.root, "tools/third_party_files.txt",
              "# path | licence | role | provenance\n" + "".join(third_party))
        for path, text in files.items():
            write(self.root, path, text)
        return repo_shape.spdx_licenses(self.root, tracked=sorted(files))

    def test_spdx_file_without_a_header_is_reported(self):
        problems = self.spdx_problems({"dcfs/x.cc": "int x;\n"})
        self.assertEqual(1, len(problems), problems)
        self.assertIn("dcfs/x.cc has no SPDX-License-Identifier", problems[0])

    def test_spdx_mit_header_passes(self):
        self.assertEqual([], self.spdx_problems(
            {"dcfs/x.cc": "// SPDX-License-Identifier: MIT\nint x;\n"}))

    def test_spdx_header_is_on_line_two_after_a_shebang(self):
        problems = self.spdx_problems({
            "tools/x.sh": "#!/bin/sh\n# SPDX-License-Identifier: MIT\n",
            "tools/z.sh": "#!/bin/sh\n# no licence\n# SPDX-License-Identifier: "
                          "MIT\n",
        })
        self.assertEqual(1, len(problems), problems)
        self.assertIn("tools/z.sh has no SPDX-License-Identifier", problems[0])

    def test_spdx_listed_bsd_passes(self):
        self.assertEqual([], self.spdx_problems(
            {"dcfs/b.cc": "// SPDX-License-Identifier: BSD-3-Clause\n"},
            third_party=[
                "dcfs/b.cc | BSD-3-Clause | shipped | copied from x\n"]))

    def test_spdx_unlisted_bsd_is_reported(self):
        problems = self.spdx_problems(
            {"dcfs/b.cc": "// SPDX-License-Identifier: BSD-3-Clause\n"})
        self.assertEqual(1, len(problems), problems)
        self.assertIn("dcfs/b.cc is BSD-3-Clause but is not listed",
                      problems[0])

    def test_spdx_listing_must_name_the_header_licence(self):
        problems = self.spdx_problems(
            {"dcfs/b.cc": "// SPDX-License-Identifier: BSD-3-Clause\n"},
            third_party=["dcfs/b.cc | ISC | shipped | copied from x\n"])
        self.assertEqual(1, len(problems), problems)
        self.assertIn("dcfs/b.cc is BSD-3-Clause, listed as ISC", problems[0])

    def test_spdx_gpl_listed_test_only_passes(self):
        self.assertEqual([], self.spdx_problems(
            {"tools/g.c": "/* SPDX-License-Identifier: GPL-2.0-only */\n"},
            third_party=["tools/g.c | GPL-2.0-only | test-only | a patch\n"]))

    def test_spdx_gpl_listed_as_shipped_is_reported(self):
        problems = self.spdx_problems(
            {"tools/g.c": "/* SPDX-License-Identifier: GPL-2.0-only */\n"},
            third_party=["tools/g.c | GPL-2.0-only | shipped | a patch\n"])
        self.assertEqual(1, len(problems), problems)
        self.assertIn("GPL-2.0-only and must have role test-only", problems[0])

    def test_spdx_licence_not_in_the_allowed_list_is_reported(self):
        problems = self.spdx_problems(
            {"dcfs/a.cc": "// SPDX-License-Identifier: AGPL-3.0-only\n"},
            third_party=["dcfs/a.cc | AGPL-3.0-only | test-only | x\n"])
        self.assertEqual(1, len(problems), problems)
        self.assertIn("under AGPL-3.0-only, which "
                      "tools/licenses_allowed.txt does not allow", problems[0])

    def test_spdx_checks_only_the_covered_files(self):
        problems = self.spdx_problems({
            "docs/x.md": "no header\n",
            "tools/style_matchers/bad.cc": "no header\n",
            "tools/mutation/fixture.cc": "no header\n",
            "dcfs/testdata/y.cc": "no header\n",
            "BUILD": "no header\n",
            "test/qemu/guest/init": "no header\n",
            "tools/z.py": "no header\n",
        })
        self.assertEqual(3, len(problems), problems)
        self.assertIn("BUILD has no", problems[0])
        self.assertIn("test/qemu/guest/init has no", problems[1])
        self.assertIn("tools/z.py has no", problems[2])


    def long_problems(self, files, allow=""):
        write(self.root, "tools/repo_shape_long_lines.txt", "# known\n" + allow)
        for path, text in files.items():
            write(self.root, path, text)
        return repo_shape.long_lines(self.root, tracked=sorted(files))

    def test_a_long_markdown_line_is_reported(self):
        problems = self.long_problems({"docs/a.md": "word " * 17 + "\n"})
        self.assertEqual(1, len(problems), problems)
        self.assertIn("docs/a.md has 1 line(s) over 80 columns, 0 known",
                      problems[0])

    def test_long_lines_in_a_fence_a_table_or_a_link_definition_pass(self):
        long = "word " * 17
        text = ("```\n" + long + "\n```\n| " + long + "|\n"
                "[ref]: https://example.com/" + "a" * 90 + "\n"
                "See https://example.com/" + "a" * 90 + "\n"
                "Read `" + "a/" * 45 + "x.cc` now.\n")
        self.assertEqual([], self.long_problems({"docs/a.md": text}))

    def test_other_extensions_and_third_party_are_scanned_or_skipped(self):
        long = "x = 1  # " + "word " * 17 + "\n"
        problems = self.long_problems({
            "tools/a.py": long, "tools/b.sh": long, "BUILD.bazel": long,
            "tools/c.bzl": long, ".bazelrc": long, "dcfs/a.cc": long,
            "third_party/p/README.md": long, "MODULE.bazel.lock": long})
        self.assertEqual(5, len(problems), problems)

    def test_a_count_above_the_long_lines_list_is_reported(self):
        files = {"docs/a.md": ("word " * 17 + "\n") * 3}
        self.assertEqual([], self.long_problems(files, "docs/a.md 3\n"))
        problems = self.long_problems(files, "docs/a.md 2\n")
        self.assertEqual(1, len(problems), problems)
        self.assertIn("3 line(s) over 80 columns, 2 known", problems[0])

    def test_a_stale_long_lines_entry_is_reported(self):
        files = {"docs/a.md": "short\n" * 2 + "word " * 17 + "\n"}
        problems = self.long_problems(files, "docs/a.md 2\n")
        self.assertEqual(1, len(problems), problems)
        self.assertIn("lists 2 for docs/a.md, which has 1", problems[0])
        problems = self.long_problems({"docs/a.md": "short\n"},
                                      "docs/a.md 1\n")
        self.assertIn("lists 1 for docs/a.md, which has 0", problems[0])

    def split_problems(self, files, allow=""):
        write(self.root, "tools/repo_shape_split_spans.txt", "# known\n" + allow)
        for path, text in files.items():
            write(self.root, path, text)
        return repo_shape.split_code_spans(self.root, tracked=sorted(files))

    def test_a_code_span_split_across_a_line_is_reported(self):
        problems = self.split_problems({"docs/a.md": "Use `foo\nbar` here.\n"})
        self.assertEqual(1, len(problems), problems)
        self.assertIn("docs/a.md has 1 split code span(s), 0 known",
                      problems[0])

    def test_whole_spans_and_fences_are_not_split_spans(self):
        text = ("Use `foo` and `bar`.\n\n```\nx `y\n```\nOk `a`\n\n"
                "``` \nz\n```\n")
        self.assertEqual([], self.split_problems({"docs/a.md": text}))

    def test_a_listed_split_span_is_accepted_and_a_stale_one_is_not(self):
        files = {"docs/a.md": "Use `foo\nbar` here.\n"}
        self.assertEqual([], self.split_problems(files, "docs/a.md 1\n"))
        problems = self.split_problems({"docs/a.md": "ok\n"}, "docs/a.md 1\n")
        self.assertEqual(1, len(problems), problems)


class RepoShapeCliTest(unittest.TestCase):
    """`python3 -I tools/repo_shape.py` runs every check the test does,
    including the ones that read git's tracked files (step 25.51)."""

    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.root = self.dir.name
        good_tree(self.root)
        write(self.root, "tools/repo_shape_long_lines.txt", "# known\n")
        write(self.root, "tools/repo_shape_split_spans.txt", "# known\n")
        write(self.root, "tools/licenses_allowed.txt", "MIT\n")
        write(self.root, "tools/third_party_files.txt", "# none\n")
        for path in sorted(self.files()):
            if repo_shape.spdx_wanted(path):
                with open(os.path.join(self.root, path), "r+") as f:
                    text = f.read()
                    f.seek(0)
                    f.write("# SPDX-License-Identifier: MIT\n" + text)

    def files(self):
        for base, _, names in os.walk(self.root):
            for name in names:
                yield os.path.relpath(os.path.join(base, name), self.root)

    def tearDown(self):
        self.dir.cleanup()

    def run_cli(self):
        subprocess.run(["git", "init", "-q"], cwd=self.root, check=True)
        subprocess.run(["git", "add", "-A"], cwd=self.root, check=True)
        done = subprocess.run(
            [sys.executable, "-I", repo_shape.__file__, self.root],
            capture_output=True, text=True, check=False)
        return done.returncode, done.stdout

    def test_a_good_tree_exits_zero(self):
        code, out = self.run_cli()
        self.assertEqual((0, ""), (code, out))

    def test_a_new_long_line_in_a_tracked_md_file_fails(self):
        write(self.root, "docs/a.md", "word " * 17 + "\n")
        code, out = self.run_cli()
        self.assertEqual((1, [
            "docs/a.md has 1 line(s) over 80 columns, 0 known: fix them "
            "(the list tools/repo_shape_long_lines.txt does not grow)"]),
                         (code, out.splitlines()))

    def test_an_allowlist_count_above_the_actual_fails(self):
        write(self.root, "docs/a.md", "short\n")
        write(self.root, "tools/repo_shape_long_lines.txt",
              "# known\ndocs/a.md 2\n")
        code, out = self.run_cli()
        self.assertEqual((1, [
            "tools/repo_shape_long_lines.txt lists 2 for docs/a.md, which "
            "has 0: lower it or take the entry off (the list only shrinks)"]),
                         (code, out.splitlines()))

    def test_a_split_backtick_span_fails(self):
        write(self.root, "docs/a.md", "Use `foo\nbar` here.\n")
        code, out = self.run_cli()
        self.assertEqual((1, [
            "docs/a.md has 1 split code span(s), 0 known: fix them (the "
            "list tools/repo_shape_split_spans.txt does not grow)"]),
                         (code, out.splitlines()))


GROUP = ('filegroup(name = "all_cxx_sources", srcs = glob(%s))\n')
TOOLS_BUILD = ('py_test(\n    name = "approved_macros_test",\n    data = [\n'
               '%s    ],\n)\n')


class ScanSourcesTest(unittest.TestCase):
    """The scan of macro definitions (//tools:approved_macros_test) reads the
    C++ of every package through its all_cxx_sources group."""

    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.root = self.dir.name
        good_tree(self.root)
        write(self.root, "dcfs/BUILD.bazel", GROUP % '["**/*.cc", "**/*.h"]')
        write(self.root, "tools/BUILD.bazel",
              TOOLS_BUILD % '        "//dcfs:all_cxx_sources",\n'
              '        "//bench:all_cxx_sources",\n')

    def tearDown(self):
        self.dir.cleanup()

    def test_a_covered_tree_is_good(self):
        self.assertEqual([], repo_shape.scan_covers_cxx(self.root))

    def test_a_package_without_the_group_is_reported(self):
        write(self.root, "bench/BUILD.bazel", "")
        write(self.root, "bench/b.cc")
        problems = repo_shape.scan_covers_cxx(self.root)
        self.assertEqual(1, len(problems), problems)
        self.assertIn("bench/b.cc", problems[0])
        self.assertIn("all_cxx_sources", problems[0])

    def test_a_new_subpackage_is_reported(self):
        write(self.root, "dcfs/sub/BUILD.bazel", "")
        write(self.root, "dcfs/sub/c.h")
        problems = repo_shape.scan_covers_cxx(self.root)
        self.assertEqual(1, len(problems), problems)
        self.assertIn("dcfs/sub/c.h", problems[0])

    def test_a_subdirectory_the_glob_does_not_reach_is_reported(self):
        write(self.root, "bench/BUILD.bazel", GROUP % '["*.cc"]')
        write(self.root, "bench/inner/d.cc")
        problems = repo_shape.scan_covers_cxx(self.root)
        self.assertEqual(1, len(problems), problems)
        self.assertIn("bench/inner/d.cc", problems[0])
        self.assertIn("**", problems[0])

    def test_a_group_the_test_does_not_read_is_reported(self):
        write(self.root, "test/qemu/BUILD.bazel", GROUP % '["*.cc"]')
        write(self.root, "test/qemu/t.cc")
        problems = repo_shape.scan_covers_cxx(self.root)
        self.assertEqual(1, len(problems), problems)
        self.assertIn("//test/qemu:all_cxx_sources", problems[0])

    def test_the_matchers_fixtures_are_not_scanned(self):
        write(self.root, "tools/style_matchers/BUILD.bazel", "")
        write(self.root, "tools/style_matchers/x_bad.cc")
        self.assertEqual([], repo_shape.scan_covers_cxx(self.root))


if __name__ == "__main__":
    unittest.main()
