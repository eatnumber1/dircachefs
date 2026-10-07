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


if __name__ == "__main__":
    unittest.main()
