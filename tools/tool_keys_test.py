"""The test tools' builds do not depend on the sanitizer configuration.

Phase 6.3 follow-up (docs/plan/phases/06-test-tiers-and-test-speed.md,
docs/plan/notes/build-speed-2026-10-07.md section 1.5): QEMU, e2fsprogs
(with libarchive), xfsprogs, btrfs-progs and their private libraries
(util-linux's libuuid/libblkid, urcu, inih, glib, pcre2, zlib) are built by
rules_foreign_cc, which writes `--copt`/`--linkopt` into the configure
script. Under `--config=asan`/`--config=ubsan` every one of those actions
therefore had a different key and re-ran (about 890 s on a cold cache), even
though the tools are not instrumented and behave identically.

This test asks Bazel directly. For each tool target it runs
`bazel aquery 'deps(<target>)'` under the plain, `--config=asan` and
`--config=ubsan` configurations and requires the same set of actions with
the same action keys in all three: a key is a digest of the command line,
the environment and the inputs' paths, so equal keys mean a cache hit, and
the output paths are part of it, so a tool whose actions moved to another
configuration directory fails too. The kernel, busybox, bc, the Debian
image and the TLC overrides jar are the positive control: they were already
independent of the sanitizer flags (a genrule's key holds only its command
and environment), and the test must keep passing for them.

The check needs a Bazel server and the whole external-repository set, so it
runs its own server (`--output_base`) inside the test's temporary
directory when TOOL_KEYS_OUTPUT_BASE is not set; setting it (to a
directory outside the test) reuses an output base and its analysis cache.
Run it directly with the workspace's server for a quick check:
`python3 tools/tool_keys_test.py --workspace=. --bazel=bazel`.
"""

import argparse
import json
import os
import subprocess
import sys
import unittest

# (name, target, is_control). Every target's whole dependency closure is
# compared, the exec-configuration bootstrap tools (m4, gawk, make, pkgconf,
# autoconf probes) included.
TARGETS = [
    ("qemu", "//third_party/qemu:qemu_system_x86_64", False),
    ("mke2fs", "//third_party/e2fsprogs:mke2fs", False),
    ("debugfs", "//third_party/e2fsprogs:debugfs", False),
    ("mkfs.xfs", "//third_party/xfsprogs:mkfs_xfs", False),
    ("mkfs.btrfs", "//third_party/btrfs-progs:mkfs_btrfs", False),
    ("kernel", "//third_party/linux:bzImage", True),
    ("busybox", "//third_party/busybox:busybox_build", True),
    ("bc", "//third_party/bc:bc", True),
    ("debian rootfs", "//third_party/debian:rootfs", True),
    ("tlc overrides jar", "//third_party/tlaplus:tlc_overrides", True),
]

CONFIGS = [
    ("plain", []),
    ("asan", ["--config=asan"]),
    ("ubsan", ["--config=ubsan"]),
]

# The foreign_cc tools' closures are thousands of actions (QEMU's alone is
# about 7,000, mostly the exec-configuration bootstrap); a genrule is one or
# two. An aquery that returns less than that is a broken query, not a pass.
MIN_ACTIONS = {"qemu": 1000, "mke2fs": 1000, "debugfs": 1000, "mkfs.xfs": 1000,
               "mkfs.btrfs": 1000}
DEFAULT_MIN_ACTIONS = 1


def path_of(fragments, fragment_id):
    parts = []
    while fragment_id:
        fragment = fragments[fragment_id]
        parts.append(fragment["label"])
        fragment_id = fragment.get("parentId", 0)
    return "/".join(reversed(parts))


def action_keys(aquery_json):
    """Maps (label, mnemonic, primary output path) to actionKey."""
    data = json.loads(aquery_json)
    labels = {t["id"]: t["label"] for t in data.get("targets", [])}
    fragments = {f["id"]: f for f in data.get("pathFragments", [])}
    outputs = {
        a["id"]: path_of(fragments, a["pathFragmentId"])
        for a in data.get("artifacts", [])
    }
    keys = {}
    for action in data.get("actions", []):
        ident = (
            labels[action["targetId"]],
            action["mnemonic"],
            outputs.get(action.get("primaryOutputId"), ""),
        )
        keys[ident] = action["actionKey"]
    return keys


class Aquery:
    def __init__(self, bazel, workspace, output_base):
        self.bazel = bazel
        self.workspace = workspace
        self.output_base = output_base

    def keys(self, config_flags, target):
        startup = [self.bazel]
        if self.output_base:
            startup.append("--output_base=" + self.output_base)
        cmd = startup + [
            "aquery",
            "--output=jsonproto",
            *config_flags,
            "deps(%s)" % target,
        ]
        proc = subprocess.run(
            cmd,
            cwd=self.workspace,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
        if proc.returncode != 0:
            raise RuntimeError(
                "%s failed (%d):\n%s" % (" ".join(cmd), proc.returncode, proc.stderr)
            )
        return action_keys(proc.stdout)

    def shutdown(self):
        if self.output_base:
            subprocess.run(
                [self.bazel, "--output_base=" + self.output_base, "shutdown"],
                cwd=self.workspace,
                check=False,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )


class ToolKeysTest(unittest.TestCase):
    aquery = None
    keys = {}

    @classmethod
    def setUpClass(cls):
        # The configuration is the outer loop: changing the flags discards
        # Bazel's analysis cache, so every target of one configuration is
        # queried before switching.
        for config, flags in CONFIGS:
            for name, target, _ in TARGETS:
                cls.keys[(config, name)] = cls.aquery.keys(flags, target)

    @classmethod
    def tearDownClass(cls):
        cls.aquery.shutdown()

    def check(self, name):
        base = self.keys[("plain", name)]
        self.assertGreaterEqual(
            len(base), MIN_ACTIONS.get(name, DEFAULT_MIN_ACTIONS), "empty aquery"
        )
        problems = []
        for config, _ in CONFIGS[1:]:
            other = self.keys[(config, name)]
            changed = sorted(i for i in base if i in other and base[i] != other[i])
            only_plain = sorted(i for i in base if i not in other)
            only_other = sorted(i for i in other if i not in base)
            if changed or only_plain or only_other:
                problems.append(
                    "%s: %d of %d actions differ from plain (%d key changes, "
                    "%d only in plain, %d only in %s)"
                    % (
                        config,
                        len(changed) + len(only_plain),
                        len(base),
                        len(changed),
                        len(only_plain),
                        len(only_other),
                        config,
                    )
                )
                for ident in (changed + only_plain)[:8]:
                    problems.append("    %s %s %s" % ident)
        if problems:
            self.fail("%s is not independent of the sanitizer flags:\n%s" % (
                name, "\n".join(problems)))


def _add_tests():
    for name, _, control in TARGETS:
        slug = name.replace(" ", "_").replace(".", "_")
        prefix = "test_control_" if control else "test_tool_"
        setattr(ToolKeysTest, prefix + slug, lambda self, n=name: self.check(n))


_add_tests()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--workspace", help="the workspace root")
    parser.add_argument(
        "--workspace-file",
        help="a source file at the workspace root (resolved through "
        "runfiles symlinks to find the root)",
    )
    parser.add_argument("--bazel", default="bazel")
    args, rest = parser.parse_known_args()
    workspace = args.workspace
    if not workspace:
        workspace = os.path.dirname(os.path.realpath(args.workspace_file))
    output_base = os.environ.get("TOOL_KEYS_OUTPUT_BASE")
    if not output_base and args.workspace_file:
        output_base = os.path.join(os.environ["TEST_TMPDIR"], "output_base")
    ToolKeysTest.aquery = Aquery(args.bazel, workspace, output_base)
    unittest.main(argv=[sys.argv[0]] + rest, verbosity=2)


if __name__ == "__main__":
    main()
