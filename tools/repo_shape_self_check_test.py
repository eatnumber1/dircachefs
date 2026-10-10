"""Self-check of the repository-shape rules (plan steps 26.1 and 26.13).

Fixture trees with a missing README, an unreferenced script, an unlisted
DISABLED_ check and a bare sleep: each rule must report it; a good tree
reports nothing.
"""

import os
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
    write(root, "test/qemu/guest/init")
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


class RepoShapeSelfCheckTest(unittest.TestCase):

    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.root = self.dir.name
        good_tree(self.root)

    def tearDown(self):
        self.dir.cleanup()

    def test_good_tree_has_no_problems(self):
        self.assertEqual([], repo_shape.all_problems(self.root))

    def test_missing_readme_is_reported(self):
        os.remove(os.path.join(self.root, "third_party/pin/README.md"))
        problems = repo_shape.third_party_readmes(self.root)
        self.assertEqual(1, len(problems), problems)
        self.assertIn("third_party/pin/", problems[0])

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


if __name__ == "__main__":
    unittest.main()
