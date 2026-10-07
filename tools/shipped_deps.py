"""Golden of the external repositories in the shipped binaries (step 26.9).

//dcfs:linked_deps (a genquery over deps(//dcfs:main) + deps(//dcfs:main_static),
also what //tools/sbom:sbom_test reads) is reduced to canonical repository
names, minus the repositories that contribute no code, and compared with
dcfs/shipped_deps.txt, one name per line.
"""

import difflib
import sys

from sbom import graph_repos

# Build configuration only, no code in the binaries (the `build_only` entries
# of tools/sbom/pins.json): Bazel's tool and toolchain configuration labels,
# platform constraints, the C++ rules.
EXCLUDED = frozenset({"bazel_tools", "platforms", "rules_cc+"})

MESSAGE = ("a new dependency in the shipped binary is a deliberate edit of "
           "dcfs/shipped_deps.txt")


def shipped_repos(graph_text):
    """Sorted canonical names of the repositories the binaries link."""
    return sorted(graph_repos(graph_text) - EXCLUDED)


def golden_repos(golden_text):
    """Sorted names in the golden; blank lines and # comments are ignored."""
    lines = (line.split("#", 1)[0].strip() for line in golden_text.splitlines())
    return sorted(line for line in lines if line)


def compare(graph_text, golden_text):
    """Returns '' when they agree, else the diff and the sentence."""
    want = golden_repos(golden_text)
    got = shipped_repos(graph_text)
    if want == got:
        return ""
    diff = "".join(difflib.unified_diff(
        [name + "\n" for name in want], [name + "\n" for name in got],
        "dcfs/shipped_deps.txt", "//dcfs:linked_deps"))
    return diff + MESSAGE + "\n"


def main(argv):
    graph, golden = argv
    with open(graph, encoding="utf-8") as f:
        graph_text = f.read()
    with open(golden, encoding="utf-8") as f:
        golden_text = f.read()
    problem = compare(graph_text, golden_text)
    sys.stderr.write(problem)
    return 1 if problem else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
