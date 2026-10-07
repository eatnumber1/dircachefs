"""Repository-shape rules (plan step 26.13).

Each function takes the root of a source tree and returns a list of problems
(empty: the rule holds):

- third_party_readmes: every third_party/<name>/ directory has a README.md;
- guest_scripts_used: every test/qemu/guest/*.sh is used by a test in
  test/qemu/BUILD.bazel, unless it is `lib.sh`, `pjdfstest_lib.sh` or
  `init`, or another guest script sources it (a helper library);
- disabled_checks_listed: every `disabled NAME ...` check of the guest
  scripts is named in README.md's Limitations section.

(The fourth rule, commit subjects, needs git: .github/ci/commit_subjects.sh.)
"""

import os
import re

NOT_TESTS = frozenset({"lib.sh", "pjdfstest_lib.sh", "init"})


def read(root, *parts):
    with open(os.path.join(root, *parts), encoding="utf-8") as f:
        return f.read()


def guest_scripts(root):
    guest = os.path.join(root, "test", "qemu", "guest")
    return sorted(n for n in os.listdir(guest) if n.endswith(".sh"))


def third_party_readmes(root):
    problems = []
    base = os.path.join(root, "third_party")
    for name in sorted(os.listdir(base)):
        if (os.path.isdir(os.path.join(base, name)) and
                not os.path.isfile(os.path.join(base, name, "README.md"))):
            problems.append(
                "third_party/%s/ has no README.md (docs/style.md, Bazel: a "
                "pin's README says what it is and how to update it)" % name)
    return problems


def guest_scripts_used(root):
    build = read(root, "test", "qemu", "BUILD.bazel")
    used = set()
    templates = []
    for path in re.findall(r'guest_script\s*=\s*"guest/([^"]+\.sh)"', build):
        used.add(path)
    for template in re.findall(r'guest_script\s*=\s*"guest/([^"]*%s[^"]*\.sh)"',
                               build):
        templates.append(re.compile(
            "^" + re.escape(template).replace("%s", "[a-z0-9_]+") + "$"))
    scripts = guest_scripts(root)
    sourced = set()
    for name in scripts:
        text = read(root, "test", "qemu", "guest", name)
        sourced.update(re.findall(
            r'^\s*\.\s+"\$\(dirname "\$0"\)/([A-Za-z0-9_.]+\.sh)"', text,
            re.MULTILINE))
    problems = []
    for name in scripts:
        if (name in NOT_TESTS or name in used or name in sourced or
                any(t.match(name) for t in templates)):
            continue
        problems.append(
            "test/qemu/guest/%s is used by no test in test/qemu/BUILD.bazel "
            "(a guest_script, or a library another guest script sources)"
            % name)
    return problems


def disabled_check_names(root):
    names = set()
    for name in guest_scripts(root):
        names.update(re.findall(
            r"^[ \t]*disabled[ \t]+([A-Za-z0-9_.-]+)", read(
                root, "test", "qemu", "guest", name), re.MULTILINE))
    return sorted(names)


def limitations(readme):
    match = re.search(r"^## Limitations\n(.*?)(?=^## |\Z)", readme,
                      re.MULTILINE | re.DOTALL)
    return match.group(1) if match else ""


def disabled_checks_listed(root):
    section = limitations(read(root, "README.md"))
    return [
        "DISABLED_%s (test/qemu/guest) is not named in README.md's "
        "Limitations section" % name
        for name in disabled_check_names(root)
        if name not in section
    ]


def all_problems(root):
    return (third_party_readmes(root) + guest_scripts_used(root) +
            disabled_checks_listed(root))
