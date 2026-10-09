"""Self-check of the repository-shape rules (plan steps 26.1 and 26.13).

Fixture trees with a missing README, an unreferenced script and an unlisted
DISABLED_ check: each rule must report it; a good tree reports nothing.
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
          'qemu_test(name = "s", guest_script = "guest/shard_%s.sh" % x)\n')
    write(root, "test/qemu/guest/lib.sh", "disabled() { :; }\n")
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


if __name__ == "__main__":
    unittest.main()
