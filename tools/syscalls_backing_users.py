"""Who may depend on //dcfs:syscalls_backing (plan step 26.7).

//dcfs:syscalls_backing_users (a genquery of the direct dependents of the
backing-reaching wrappers) is compared with dcfs/syscalls_backing_users.txt,
one label per line (blank lines and # comments ignored). A new dependent is a
deliberate edit of the golden: it is the build-graph form of the layering rule
of docs/style.md 1.8 (only backing.cc, its lower layers, startup and the tests
reach the backing filesystem through syscalls).
"""

import difflib

MESSAGE = ("a target that depends on //dcfs:syscalls_backing is a deliberate "
           "edit of dcfs/syscalls_backing_users.txt (docs/style.md 1.8: only "
           "backing, its lower layers, startup and tests may reach the "
           "backing filesystem through syscalls)")


def labels(text):
    """Sorted labels of a genquery output or a golden."""
    found = set()
    for line in text.splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            found.add(line.lstrip("@"))
    return sorted(found)


def compare(graph_text, golden_text):
    """"" when the dependents equal the golden, else a diff and MESSAGE."""
    graph, golden = labels(graph_text), labels(golden_text)
    if graph == golden:
        return ""
    diff = "\n".join(difflib.unified_diff(
        golden, graph, "dcfs/syscalls_backing_users.txt", "the build graph",
        lineterm="", n=0))
    return diff + "\n" + MESSAGE
