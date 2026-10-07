#!/usr/bin/env python3
"""A small mutation tester for the protocol code (step 26.5).

  mutate.py generate --workspace W --scope scope.txt --out mutants.json
  mutate.py run      --workspace W --mutants mutants.json --result result.json
                     [--sample N] [--seed S] [--only ID,ID] [--shard K/N]
                     [--killers PHASES] [--fail-on-survivor]
                     [--survivors-out FILE]
  mutate.py changed  --range A..B [--max-mutants 30] --result result.json
                     [--fail-on-survivor] [--survivors-out FILE]

`generate` finds the places to mutate with clang's AST (-ast-dump=json, the
pinned clang, the file's own compile command from `bazel aquery`): inside the
functions scope.txt selects it negates the condition of an if/while/for/?:,
deletes a call to a Begin*/End*/Mark* function that returns absl::Status or
void, and swaps present/absent (kFound/kNegative, kPresent/kAbsent). `run`
applies one mutant at a time in a scratch copy of the tree (its own Bazel
output base; the disk cache makes unchanged actions free) and runs the
killers (the small tier of //dcfs/... and the trace validation) with the
first failure ending the mutant. A mutant that does not compile is `invalid`,
one a test fails or times out on is `killed`, the rest `survived`: a missing
test. See README.md. Run it with `bazel run //tools/mutation:mutate`.

Exit status of `run` and `changed`: 0 (survivors are findings, listed in the
output and --survivors-out), 1 with --fail-on-survivor when one survived, 2 on
a tooling error (a mutant run that was neither killed, survived nor invalid,
or a generator failure): the scheduled job fails only on 2, the per-push step
on 1 and 2.

`changed` is the per-push mode: only the functions whose lines the commit
range touches (git diff hunks mapped to the AST's functions) are mutated, at
most --max-mutants of them (a sample with a fixed seed beyond that).
"""

import argparse
import json
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
import time

FUNCTION_KINDS = {"FunctionDecl", "CXXMethodDecl", "CXXConstructorDecl",
                  "CXXDestructorDecl"}
LOOP_KINDS = {"IfStmt": "if", "WhileStmt": "while", "ForStmt": "for",
              "ConditionalOperator": "?:"}
PHASE_CALL = re.compile(r"^(Begin|End|Mark)[A-Za-z]*$")
SWAPS = {"kFound": "kNegative", "kNegative": "kFound",
         "kPresent": "kAbsent", "kAbsent": "kPresent"}


# ---- scope -----------------------------------------------------------------

def read_scope(path):
    """Returns [{file, target, rules: [(mode, args)]}]."""
    files = []
    for line in open(path, encoding="utf-8"):
        line = line.split("#", 1)[0].rstrip()
        if not line.strip():
            continue
        words = line.split()
        if words[0] == "file":
            files.append({"file": words[1], "target": words[2], "rules": []})
        elif words[0] in ("all", "names", "calls"):
            files[-1]["rules"].append((words[0], words[1:]))
        else:
            raise SystemExit("scope.txt: cannot read %r" % line)
    return files


# ---- AST ------------------------------------------------------------------

class Walker:
    """Walks the concatenated JSON documents of one -ast-dump=json run.

    The dump prints a `file` only when it changes from the previous location
    it printed, so the current file is tracked in document order.
    """

    def __init__(self, main_file):
        self.main = main_file
        self.cur = None
        self.functions = []  # (name, body node, in_main)

    def note(self, loc):
        if not isinstance(loc, dict):
            return
        if "spellingLoc" in loc:
            self.note(loc["spellingLoc"])
            self.note(loc["expansionLoc"])
            return
        if "file" in loc:
            self.cur = loc["file"]

    def in_main(self):
        return self.cur is not None and os.path.normpath(self.cur).endswith(
            os.path.normpath(self.main))

    def visit(self, node, out):
        self.note(node.get("loc"))
        rng = node.get("range")
        here = self.in_main()
        if rng:
            self.note(rng.get("begin"))
            self.note(rng.get("end"))
        node["_main"] = here and self.in_main()
        if node.get("kind") in FUNCTION_KINDS:
            body = [c for c in node.get("inner", []) if c.get("kind") == "CompoundStmt"]
            if body and node["_main"]:
                out.append(node)
        for child in node.get("inner", []):
            self.visit(child, out)


def parse_stream(data, main_file):
    walker = Walker(main_file)
    dec = json.JSONDecoder()
    pos = 0
    n = len(data)
    functions = []
    while True:
        while pos < n and data[pos] in " \r\n\t":
            pos += 1
        if pos >= n:
            break
        doc, pos = dec.raw_decode(data, pos)
        walker.visit(doc, functions)
        # Keep only what mutation needs: drop the other documents at once.
        if not any(f is doc or _contains(doc, f) for f in functions[-1:]):
            pass
    return functions


def _contains(doc, node):
    return False


def span(node):
    """(start, end) byte offsets of an expression in the main file, or None."""
    rng = node.get("range")
    if not rng:
        return None
    b, e = rng["begin"], rng["end"]
    if "spellingLoc" in b or "spellingLoc" in e:
        if "spellingLoc" not in b or "spellingLoc" not in e:
            return None
        if b["expansionLoc"].get("offset") != e["expansionLoc"].get("offset"):
            return None
        b, e = b["spellingLoc"], e["spellingLoc"]
    if "offset" not in b or "offset" not in e:
        return None
    return b["offset"], e["offset"] + e.get("tokLen", 0), b.get("tokLen", 0)


TOKEN = re.compile(r"""[A-Za-z_]\w*|\d[\w.']*|"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|::|->|&&|\|\||[-+*/%&|^!<>=]=?|.""", re.S)


def sane(sp, source):
    """True when the span starts at a token of the length clang says: the
    AST dump omits a `file` that did not change, and a node of a header can
    be taken for one of the main file; its offsets then land mid-text."""
    start, end, toklen = sp
    if start >= len(source) or source[start].isspace():
        return False
    m = TOKEN.match(source, start)
    return bool(m) and len(m.group(0)) == toklen and end > start


def callee_name(call):
    node = call.get("inner", [None])[0]
    while node is not None:
        if node.get("kind") == "DeclRefExpr":
            return node.get("referencedDecl", {}).get("name")
        if node.get("kind") == "MemberExpr":
            return node.get("name")
        inner = node.get("inner")
        node = inner[0] if inner else None
    return None


def collect_calls(node, acc):
    if node.get("kind") in ("CallExpr", "CXXMemberCallExpr"):
        name = callee_name(node)
        if name:
            acc.append(name)
    for c in node.get("inner", []):
        collect_calls(c, acc)


def selected(fn, rules):
    name = fn.get("name", "")
    for mode, args in rules:
        if mode == "all":
            return True
        if mode == "names" and name in args:
            return True
        if mode == "calls":
            calls = []
            collect_calls(fn, calls)
            if any(re.search(args[0], c) for c in calls):
                return True
    return False


def mutants_in(fn, source):
    """Yields mutant dicts for the body of function node fn."""
    found = []

    def walk(node):
        kind = node.get("kind")
        inner = node.get("inner", [])
        if node.get("_main"):
            if kind in LOOP_KINDS:
                idx = 0
                if kind == "IfStmt":
                    idx = int(bool(node.get("hasInit"))) + int(bool(node.get("hasVar")))
                    if node.get("hasVar"):
                        idx = None
                elif kind == "WhileStmt":
                    idx = 0 if not node.get("hasVar") else None
                elif kind == "ForStmt":
                    idx = 1  # init, condvar, cond, inc, body (empty ones are null nodes)
                    idx = 2 if len(inner) >= 3 else None
                if idx is not None and idx < len(inner):
                    cond = inner[idx]
                    sp = span(cond)
                    if sp and not sane(sp, source):
                        sp = None
                    if sp and cond.get("kind"):
                        text = source[sp[0]:sp[1]]
                        if kind == "ForStmt" and cond.get("kind") == "NullStmt":
                            sp = None
                        if sp:
                            found.append(("negate-%s" % LOOP_KINDS[kind], sp[:2],
                                          "!(" + text + ")", text))
            elif kind in ("CallExpr", "CXXMemberCallExpr"):
                name = callee_name(node)
                qt = node.get("type", {}).get("qualType", "")
                if name and PHASE_CALL.match(name) and qt in ("absl::Status", "void"):
                    sp = span(node)
                    if sp and sane(sp, source):
                        found.append(("delete-call %s" % name, sp[:2],
                                      "absl::OkStatus()" if qt == "absl::Status" else "(void)0",
                                      source[sp[0]:sp[1]]))
            elif kind == "DeclRefExpr":
                ref = node.get("referencedDecl", {})
                if ref.get("kind") == "EnumConstantDecl" and ref.get("name") in SWAPS:
                    sp = span(node)
                    if sp and sane(sp, source):
                        text = source[sp[0]:sp[1]]
                        if text.endswith(ref["name"]):
                            found.append(("swap %s/%s" % (ref["name"], SWAPS[ref["name"]]), sp[:2],
                                          text[:-len(ref["name"])] + SWAPS[ref["name"]], text))
        for c in inner:
            walk(c)

    for c in fn.get("inner", []):
        if c.get("kind") == "CompoundStmt":
            walk(c)
    return found


# ---- changed code ---------------------------------------------------------

HUNK = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@")


def parse_hunks(diff):
    """The new-file line ranges [(first, last)] a `git diff -U0` touches."""
    ranges = []
    for line in diff.splitlines():
        m = HUNK.match(line)
        if not m:
            continue
        first = int(m.group(1))
        count = 1 if m.group(2) is None else int(m.group(2))
        # A pure deletion (count 0) sits between lines first and first + 1.
        ranges.append((first, first + count - 1) if count else (first, first + 1))
    return ranges


def touches(fn_lines, hunks):
    first, last = fn_lines
    return any(a <= last and b >= first for a, b in hunks)


ZEROS = "0" * 40


def normalize_range(rev_range):
    """A push of a new branch has an all-zero base (or none): diff the tip
    against its parent then."""
    base, _, tip = rev_range.partition("..")
    if not tip:
        return rev_range
    if not base or base == ZEROS:
        return "%s~1..%s" % (tip, tip)
    return rev_range


def changed_hunks(workspace, rev_range, path):
    rev_range = normalize_range(rev_range)
    diff = subprocess.run(["git", "diff", "-U0", "--no-color", rev_range, "--", path],
                          cwd=workspace, capture_output=True, text=True, check=True).stdout
    return parse_hunks(diff)


def function_lines(fn, source):
    """(first line, last line) of a function node, 1-based, or None."""
    sp = span(fn)
    if sp is None:
        return None
    return source.count("\n", 0, sp[0]) + 1, source.count("\n", 0, sp[1]) + 1


# ---- generate -------------------------------------------------------------

def sh(cmd, cwd=None, check=True):
    return subprocess.run(cmd, cwd=cwd, capture_output=True, check=check)


def bazel(workspace, *args, check=True):
    return subprocess.run(["bazel", *args], cwd=workspace, capture_output=True,
                          check=check)


def compile_command(workspace, target, source):
    aq = bazel(workspace, "aquery", "--output=jsonproto",
               "mnemonic(CppCompile, %s)" % target).stdout
    for a in json.loads(aq)["actions"]:
        args = a["arguments"]
        if source in args:
            out, i = [], 0
            while i < len(args):
                if args[i] == "-MD":
                    i += 1
                elif args[i] in ("-MF", "-o", "-c"):
                    i += 2
                else:
                    out.append(args[i])
                    i += 1
            return out
    raise SystemExit("no compile action for %s in %s" % (source, target))


def generate(args):
    args.changed = getattr(args, "changed", None)
    workspace = os.path.abspath(args.workspace)
    execroot = bazel(workspace, "info", "execution_root").stdout.decode().strip()
    mutants = []
    for entry in read_scope(args.scope):
        source_path = os.path.join(workspace, entry["file"])
        source = open(source_path, "rb").read().decode("latin-1")  # offsets are bytes
        base = compile_command(workspace, entry["target"], entry["file"])
        # Make sure the sources and generated headers are in the execroot.
        bazel(workspace, "build", "--check_up_to_date", entry["target"], check=False)
        cmd = base + ["-fsyntax-only", "-Xclang", "-ast-dump=json",
                      "-Xclang", "-ast-dump-filter=dcfs::", "-x", "c++", entry["file"]]
        t = time.time()
        p = subprocess.run(cmd, cwd=execroot, capture_output=True)
        if p.returncode != 0:
            sys.stderr.write(p.stderr.decode()[-2000:])
            raise SystemExit("clang failed on %s" % entry["file"])
        print("%s: AST dump %d MB in %.0fs" % (entry["file"], len(p.stdout) >> 20,
                                               time.time() - t), file=sys.stderr)
        functions = parse_stream(p.stdout.decode("utf-8", "replace"), entry["file"])
        seen = set()
        hunks = None
        if args.changed:
            hunks = changed_hunks(workspace, args.changed, entry["file"])
        for fn in functions:
            if not selected(fn, entry["rules"]):
                continue
            if hunks is not None:
                lines = function_lines(fn, source)
                if lines is None or not touches(lines, hunks):
                    continue
            for op, (s, e), repl, before in mutants_in(fn, source):
                key = (entry["file"], s, e, repl)
                if key in seen:
                    continue
                seen.add(key)
                line = source.count("\n", 0, s) + 1
                mutants.append({"id": len(mutants) + 1, "file": entry["file"],
                                "function": fn.get("name"), "line": line, "op": op,
                                "start": s, "end": e, "replacement": repl,
                                "before": before})
    json.dump(mutants, open(args.out, "w"), indent=1)
    by = {}
    for m in mutants:
        by[m["op"].split()[0]] = by.get(m["op"].split()[0], 0) + 1
    print("%d mutants: %s" % (len(mutants), by), file=sys.stderr)


# ---- run ------------------------------------------------------------------

def copy_tree(workspace, dest):
    files = subprocess.run(["git", "ls-files", "-co", "--exclude-standard", "-z"],
                           cwd=workspace, capture_output=True, check=True).stdout
    tar = subprocess.Popen(["tar", "--null", "-T", "-", "-cf", "-"], cwd=workspace,
                           stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    ext = subprocess.Popen(["tar", "-x", "-C", dest], stdin=tar.stdout)
    tar.stdin.write(files)
    tar.stdin.close()
    ext.wait()
    tar.wait()
    rc = os.path.join(workspace, "user.bazelrc")
    if os.path.exists(rc):
        shutil.copy(rc, dest)


def shard_of(mutants, k, n):
    """The k-th (1-based) of n contiguous slices of the id-ordered mutants."""
    mutants = sorted(mutants, key=lambda m: m["id"])
    return mutants[(k - 1) * len(mutants) // n:k * len(mutants) // n]


def exit_code(results, fail_on_survivor):
    """2 on a tooling error, 1 on a survivor if asked to, else 0."""
    if any(r["status"] == "error" for r in results):
        return 2
    if fail_on_survivor and any(r["status"] == "survived" for r in results):
        return 1
    return 0


def run(args):
    workspace = os.path.abspath(args.workspace)
    mutants = json.load(open(args.mutants))
    if args.only:
        wanted = {int(x) for x in args.only.split(",")}
        mutants = [m for m in mutants if m["id"] in wanted]
    if args.sample and args.sample < len(mutants):
        random.Random(args.seed).shuffle(mutants)
        mutants = sorted(mutants[:args.sample], key=lambda m: m["id"])
    if args.shard:
        k, n = (int(x) for x in args.shard.split("/"))
        mutants = shard_of(mutants, k, n)
    scratch = tempfile.mkdtemp(prefix="dcfs-mutate.")
    src = os.path.join(scratch, "src")
    os.makedirs(src)
    copy_tree(workspace, src)
    # Phases, in order; a mutant that survives one goes on to the next. (A
    # size filter such as --config=fast applies to every target of its
    # invocation, so trace validation, a medium test, needs a phase of its own.)
    phases = [ph.split() for ph in args.killers.split(";")]
    results = []
    started = time.time()
    try:
        for i, m in enumerate(mutants, 1):
            path = os.path.join(src, m["file"])
            original = open(path, "rb").read()
            text = original.decode("latin-1")
            if text[m["start"]:m["end"]] != m["before"]:
                raise SystemExit("mutant %d no longer matches %s (regenerate)" % (m["id"], m["file"]))
            mutated = text[:m["start"]] + m["replacement"] + text[m["end"]:]
            open(path, "wb").write(mutated.encode("latin-1"))
            t = time.time()
            rc, tail = 0, ""
            try:
                for killers in phases:
                    try:
                        p = subprocess.run(
                            [args.bazel, "test", "--notest_keep_going", "--test_output=errors",
                             "--jobs=2", "--local_test_jobs=2"] + killers,
                            cwd=src, capture_output=True, timeout=args.timeout)
                        rc = p.returncode
                        tail = (p.stdout + p.stderr).decode("utf-8", "replace")
                    except subprocess.TimeoutExpired:
                        rc, tail = 3, "mutant run timed out (counted as killed: a hang)"
                    if rc != 0:
                        break
            finally:
                open(path, "wb").write(original)
            # Bazel: 0 all passed, 3 a test failed or timed out, 1 a build
            # error (the mutant does not compile), 4 no test ran.
            status = {0: "survived", 3: "killed", 1: "invalid"}.get(rc, "error")
            killer = ""
            if status == "killed":
                f = re.findall(r"^(?:FAIL|TIMEOUT):\s+(//\S+)", tail, re.M)
                killer = f[0] if f else ""
            r = dict(m, status=status, seconds=round(time.time() - t, 1), killer=killer)
            if status == "error":
                r["tail"] = tail[-1500:]
            if args.show_output and status == "killed":
                failing = [l for l in tail.splitlines() if re.search(
                    r"\[  FAILED  \]|Failure|Expected|Actual|Value of|FAIL|failed", l)]
                print("\n".join("    | " + l[:200] for l in failing[:12]), file=sys.stderr)
            results.append(r)
            print("[%d/%d] mutant %d %s:%d %s: %s (%.0fs)%s" % (
                i, len(mutants), m["id"], m["file"], m["line"], m["op"], status,
                r["seconds"], " by " + killer if killer else ""), file=sys.stderr)
            json.dump(results, open(args.result, "w"), indent=1)
    finally:
        subprocess.run([args.bazel, "shutdown"], cwd=src, capture_output=True)
        shutil.rmtree(scratch, ignore_errors=True)
    wall = time.time() - started
    count = lambda s: sum(1 for r in results if r["status"] == s)
    print("mutants %d: killed %d, survived %d, invalid %d, error %d; wall %.0fs (%.0fs each)" % (
        len(results), count("killed"), count("survived"), count("invalid"), count("error"),
        wall, wall / max(len(results), 1)), file=sys.stderr)
    lines = []
    for r in results:
        if r["status"] == "survived":
            lines.append("SURVIVOR %s:%d in %s: %s: `%s` -> `%s`" % (
                r["file"], r["line"], r["function"], r["op"], r["before"][:70].replace("\n", " "),
                r["replacement"][:70].replace("\n", " ")))
        elif r["status"] == "error":
            lines.append("ERROR mutant %d %s:%d: %s" % (r["id"], r["file"], r["line"],
                                                      r.get("tail", "")[-300:].replace("\n", " ")))
    print("\n".join(lines))
    if args.survivors_out:
        with open(args.survivors_out, "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + ("\n" if lines else ""))
    return exit_code(results, args.fail_on_survivor)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)
    workspace = os.environ.get("BUILD_WORKSPACE_DIRECTORY", ".")
    scope = os.path.join(workspace, "tools/mutation/scope.txt")
    g = sub.add_parser("generate")
    g.add_argument("--workspace", default=workspace)
    g.add_argument("--scope", default=scope)
    g.add_argument("--out", required=True)
    g.add_argument("--changed", default="", metavar="RANGE",
                   help="only functions whose lines this git range touches")
    r = sub.add_parser("run")
    r.add_argument("--workspace", default=workspace)
    r.add_argument("--mutants", required=True)
    r.add_argument("--result", required=True)
    r.add_argument("--sample", type=int, default=0)
    r.add_argument("--seed", type=int, default=1)
    r.add_argument("--only", default="")
    r.add_argument("--shard", default="", metavar="K/N",
                   help="the K-th of N contiguous id ranges (the scheduled job's matrix)")
    r.add_argument("--fail-on-survivor", action="store_true")
    r.add_argument("--survivors-out", default="")
    r.add_argument("--bazel", default="bazel")
    r.add_argument("--timeout", type=int, default=1800)
    r.add_argument("--show-output", action="store_true",
                   help="print the failing lines of the test that killed a mutant")
    r.add_argument("--killers", default="--config=fast //dcfs/...;//dcfs:dir_cache_fs_trace_test")
    c = sub.add_parser("changed", help="per-push mode (see the docstring)")
    c.add_argument("--workspace", default=workspace)
    c.add_argument("--scope", default=scope)
    c.add_argument("--range", required=True, dest="changed")
    c.add_argument("--max-mutants", type=int, default=30)
    c.add_argument("--seed", type=int, default=1)
    c.add_argument("--result", required=True)
    c.add_argument("--fail-on-survivor", action="store_true")
    c.add_argument("--survivors-out", default="")
    c.add_argument("--timeout", type=int, default=1800)
    c.add_argument("--bazel", default="bazel")
    c.add_argument("--killers", default="--config=fast //dcfs/...;//dcfs:dir_cache_fs_trace_test")
    a = ap.parse_args()
    if a.cmd == "generate":
        generate(a)
        return 0
    if a.cmd == "changed":
        a.out = a.result + ".mutants.json"
        generate(a)
        if not json.load(open(a.out)):
            print("no mutants: the range touches no function in scope")
            return 0
        a.mutants, a.only, a.shard = a.out, "", ""
        a.sample = a.max_mutants
        a.show_output = False
        return run(a)
    return run(a)


if __name__ == "__main__":
    sys.exit(main())
