#!/usr/bin/env python3
"""A small mutation tester for the protocol code (steps 26.5, 26.5d).

  mutate.py generate --workspace W --scope scope.txt --out mutants.json
                     [--seed S] [--per-function N] [--all]
  mutate.py run      --workspace W --mutants mutants.json --result result.json
                     [--sample N] [--seed S] [--only ID,ID] [--shard K/N]
                     [--killers PHASES] [--fail-on-survivor]
                     [--survivors-out FILE] [--summary-out FILE]
  mutate.py changed  --range A..B [--max-mutants 30] --result result.json
                     [--fail-on-survivor] [--survivors-out FILE]
  mutate.py report   RESULT.json...

`generate` finds the places to mutate with clang's AST (-ast-dump=json, the
pinned clang, the file's own compile command from `bazel aquery`). The
operators (operators.py: relational and logical replacement, negation,
constant nudges, statement deletion, status-return replacement, argument
swap, enum swap) look at the functions scope.txt selects, minus the arid
nodes of arid.txt (logging, diagnostics, testonly hooks; arid.py). Then it
samples: at most one mutant per source line and at most --per-function per
function, the choice a hash of --seed (printed) and the mutant's stable id,
so a mutant keeps its fate when unrelated code changes.

`run` applies one mutant at a time in a scratch copy of the tree (its own
Bazel output base; the disk cache makes unchanged actions free) and runs the
killers (the small tier of //dcfs/... and the trace validation) with the
first failure ending the mutant. A mutant that does not compile is `invalid`,
one a test fails or times out on is `killed`, the rest `survived`: a missing
test. A mutant listed in equivalent.txt (with its reason) is `suppressed`
and not run. See README.md. Run it with `bazel run //tools/mutation:mutate`.

Exit status of `run` and `changed`: 0 (survivors are findings, listed in the
output and --survivors-out), 1 with --fail-on-survivor when one survived, 2 on
a tooling error (a mutant run that was neither killed, survived nor invalid,
or a generator failure): the scheduled job fails only on 2, the per-push step
on 1 and 2.

`changed` is the per-push mode: only the functions whose lines the commit
range touches (git diff hunks mapped to the AST's functions) are mutated, at
most --max-mutants of them (deterministic selection by the same hash, the
operators taking turns; the surplus is reported as "not run").

`report` merges result files and prints the per-operator table and the
survivors grouped by function (the scheduled job's summary).
"""

import argparse
import collections
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

import arid as arid_lib
import operators

FUNCTION_KINDS = operators.FUNCTION_KINDS
PHASE_CALL = operators.PHASE_CALL
TOKEN = operators.TOKEN
span = operators.span
sane = operators.sane
callee_name = operators.callee_name

DEFAULT_SEED = 1
# The scheduled sweep's per-function bound and the per-push budget.
DEFAULT_PER_FUNCTION = 2
DEFAULT_BUDGET = 30
STATUSES = ("killed", "survived", "invalid", "suppressed", "error")


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
            body = [c for c in node.get("inner", [])
                    if c.get("kind") == "CompoundStmt"]
            if body and node["_main"]:
                out.append(node)
        for child in node.get("inner", []):
            self.visit(child, out)


def parse_stream(data, main_file):
    """The function nodes (with bodies) of `main_file` in an AST dump."""
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
    return functions


def ast_dump_command(base, source_file, name_filter="dcfs::"):
    """The clang command that prints the AST of the functions whose name
    starts with `name_filter` as JSON (production and the tests alike)."""
    return base + ["-fsyntax-only", "-Xclang", "-ast-dump=json",
                   "-Xclang", "-ast-dump-filter=" + name_filter,
                   "-x", "c++", source_file]


def collect_calls(node, acc):
    if node.get("kind") in operators.CALL_KINDS:
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


# ---- the mutants of a file -------------------------------------------------

def normalize(text):
    """Whitespace runs collapsed: the form that goes into a stable id."""
    return " ".join(text.split())


def stable_key(m):
    """The id of a mutant that survives edits elsewhere in the file: file,
    function, operator, before and after text, never line numbers."""
    return " | ".join([m["file"], m["function"] or "", m["op"],
                       normalize(m["before"]), normalize(m["replacement"])])


def collect_mutants(functions, source, path, rules, arid=None, hunks=None):
    """Every mutant of the selected functions, before sampling.

    `hunks` (the changed lines) restricts it to the functions they touch;
    `arid` (arid.Arid) removes the sites in arid nodes and functions. A
    mutant has `key` (stable_key) and `nth`, the order among the mutants of
    its function with the same key (so equivalent.txt can name one of two
    identical deletions).
    """
    mutants, seen = [], set()
    for fn in functions:
        if not selected(fn, rules):
            continue
        if arid and arid.function_reason(fn.get("name")):
            continue
        if hunks is not None:
            lines = function_lines(fn, source)
            if lines is None or not touches(lines, hunks):
                continue
        arid_spans = arid.spans(fn, source) if arid else ()
        counts = collections.Counter()
        for site in operators.sites_in(fn, source):
            if arid_lib.overlaps(site, arid_spans):
                continue
            ident = (path, site.start, site.end, site.replacement)
            if ident in seen:
                continue
            seen.add(ident)
            m = {"file": path, "function": fn.get("name"),
                 "line": source.count("\n", 0, site.start) + 1,
                 "operator": site.operator, "op": site.op,
                 "start": site.start, "end": site.end,
                 "replacement": site.replacement, "before": site.before}
            m["key"] = stable_key(m)
            counts[m["key"]] += 1
            m["nth"] = counts[m["key"]]
            mutants.append(m)
    return mutants


# ---- sampling ----------------------------------------------------------------

def rank(seed, m):
    """A mutant's place in the seeded order: a hash of the seed and the
    mutant's stable id (and its `nth`), so it does not move when code
    elsewhere changes."""
    text = "%s:%s#%d" % (seed, m["key"], m.get("nth", 1))
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def take_turns(mutants, seed):
    """The mutants in the seeded order with the operators taking turns, so
    that a cut keeps every operator represented."""
    groups = collections.defaultdict(list)
    for m in mutants:
        groups[m["operator"]].append(m)
    queues = [sorted(g, key=lambda m: rank(seed, m))
              for _, g in sorted(groups.items())]
    ordered = []
    while any(queues):
        for q in queues:
            if q:
                ordered.append(q.pop(0))
    return ordered


def one_per_line(mutants, seed):
    """The lowest-ranked mutant of each source line."""
    best = {}
    for m in mutants:
        k = (m["file"], m["line"])
        if k not in best or rank(seed, m) < rank(seed, best[k]):
            best[k] = m
    return list(best.values())


def sample(mutants, seed, per_function=0):
    """At most one mutant per source line and `per_function` per function
    (0: no bound), in source order."""
    kept = one_per_line(mutants, seed)
    if per_function:
        by_function = collections.defaultdict(list)
        for m in kept:
            by_function[(m["file"], m["function"])].append(m)
        kept = [m for g in by_function.values()
                for m in take_turns(g, seed)[:per_function]]
    return sorted(kept, key=lambda m: (m["file"], m["start"], m["end"]))


def pick_budget(mutants, budget, seed):
    """(chosen, not_run): the first `budget` mutants of the seeded order."""
    ordered = take_turns(mutants, seed)
    chosen = sorted(ordered[:budget], key=lambda m: (m["file"], m["start"]))
    return chosen, len(ordered) - len(chosen)


# ---- equivalent mutants ---------------------------------------------------

class EquivalentError(Exception):
    """equivalent.txt is malformed."""


REQUIRED = ("file", "function", "op", "before", "after", "reason")


def parse_equivalent(text):
    """The entries of equivalent.txt: one JSON object per line (a `#` line
    is a comment) with file, function, op, before, after and a reason, and
    optionally `nth` (which of several identical mutants of the function)."""
    entries = []
    for number, line in enumerate(text.splitlines(), 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        try:
            entry = json.loads(line)
        except ValueError as e:
            raise EquivalentError("equivalent.txt:%d: %s" % (number, e))
        for field in REQUIRED:
            if not isinstance(entry.get(field), str) or not entry[
                    field].strip():
                raise EquivalentError(
                    "equivalent.txt:%d: %r needs a non-empty `%s`"
                    % (number, line[:60], field))
        entries.append(entry)
    return entries


def load_equivalent(path):
    if not path or not os.path.exists(path):
        return []
    with open(path, encoding="utf-8") as f:
        return parse_equivalent(f.read())


def suppression(m, entries):
    """The reason a mutant is a known equivalent, or None."""
    for e in entries:
        if (e["file"] == m["file"] and e["function"] == m["function"]
                and e["op"] == m["op"]
                and normalize(e["before"]) == normalize(m["before"])
                and normalize(e["after"]) == normalize(m["replacement"])
                and e.get("nth", m.get("nth", 1)) == m.get("nth", 1)):
            return e["reason"]
    return None


def split_suppressed(mutants, entries):
    """(live, suppressed): the mutants to run and those equivalent.txt
    lists (each given its `reason`)."""
    live, suppressed = [], []
    for m in mutants:
        reason = suppression(m, entries)
        if reason:
            suppressed.append(dict(m, reason=reason))
        else:
            live.append(m)
    return live, suppressed


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
        ranges.append((first, first + count - 1) if count
                      else (first, first + 1))
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
    diff = subprocess.run(
        ["git", "diff", "-U0", "--no-color", rev_range, "--", path],
        cwd=workspace, capture_output=True, text=True, check=True).stdout
    return parse_hunks(diff)


def function_lines(fn, source):
    """(first line, last line) of a function node, 1-based, or None."""
    sp = span(fn)
    if sp is None:
        return None
    return source.count("\n", 0, sp[0]) + 1, source.count("\n", 0, sp[1]) + 1


# ---- generate -------------------------------------------------------------

def bazel(workspace, *args, check=True):
    return subprocess.run(["bazel", *args], cwd=workspace,
                          capture_output=True, check=check)


def prerequisites_args(target):
    """The bazel command that builds what compiling the target's sources
    needs and nothing else: generated headers (libfuse_config.h, ...) and
    the external sources, not the objects."""
    return ["build", "--output_groups=compilation_prerequisites_INTERNAL_",
            target]


def build_prerequisites(workspace, target):
    """Build the target's generated headers; a failure is a tooling error."""
    p = bazel(workspace, *prerequisites_args(target), check=False)
    if p.returncode != 0:
        sys.stderr.write(p.stderr.decode(errors="replace")[-2000:])
        raise SystemExit(
            "bazel could not build the compile prerequisites of %s" % target)


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


def candidates(args):
    """Every mutant of the scope, unsampled (restricted to the functions
    the git range `args.changed` touches, if any)."""
    workspace = os.path.abspath(args.workspace)
    execroot = bazel(workspace, "info",
                     "execution_root").stdout.decode().strip()
    arid = arid_lib.Arid.load(args.arid) if args.arid else None
    mutants = []
    for entry in read_scope(args.scope):
        source_path = os.path.join(workspace, entry["file"])
        # Offsets are bytes.
        source = open(source_path, "rb").read().decode("latin-1")
        base = compile_command(workspace, entry["target"], entry["file"])
        # The sources and generated headers must be in the execroot before
        # clang runs (a cold checkout has none of them).
        build_prerequisites(workspace, entry["target"])
        t = time.time()
        p = subprocess.run(ast_dump_command(base, entry["file"]),
                           cwd=execroot, capture_output=True)
        if p.returncode != 0:
            sys.stderr.write(p.stderr.decode()[-2000:])
            raise SystemExit("clang failed on %s" % entry["file"])
        print("%s: AST dump %d MB in %.0fs" % (
            entry["file"], len(p.stdout) >> 20, time.time() - t),
            file=sys.stderr)
        functions = parse_stream(p.stdout.decode("utf-8", "replace"),
                                 entry["file"])
        hunks = None
        if args.changed:
            hunks = changed_hunks(workspace, args.changed, entry["file"])
        mutants.extend(collect_mutants(functions, source, entry["file"],
                                       entry["rules"], arid, hunks))
    return mutants


def number(mutants):
    for i, m in enumerate(mutants, 1):
        m["id"] = i
    return mutants


def count_by(mutants, field="operator"):
    return dict(sorted(collections.Counter(m[field] for m in mutants).items()))


def generate(args):
    args.changed = getattr(args, "changed", None)
    args.all = getattr(args, "all", False)
    print("seed %d (--seed), per function %d (--per-function)" % (
        args.seed, args.per_function), file=sys.stderr)
    found = candidates(args)
    mutants = number(found if args.all else sample(found, args.seed,
                                                   args.per_function))
    json.dump(mutants, open(args.out, "w"), indent=1)
    print("%d candidates, %d sampled: %s" % (
        len(found), len(mutants), count_by(mutants)), file=sys.stderr)


# ---- run ------------------------------------------------------------------

def copy_tree(workspace, dest):
    files = subprocess.run(
        ["git", "ls-files", "-co", "--exclude-standard", "-z"],
        cwd=workspace, capture_output=True, check=True).stdout
    tar = subprocess.Popen(["tar", "--null", "-T", "-", "-cf", "-"],
                           cwd=workspace, stdin=subprocess.PIPE,
                           stdout=subprocess.PIPE)
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


# ---- reporting ------------------------------------------------------------

def operator_of(r):
    return r.get("operator") or r["op"].split()[0]


def summarize(results, wall=None):
    """{operators: {name: {status: n}}, total: {status: n}, run, seconds,
    per_hour}: `seconds` is the time spent on mutants that ran, and
    `per_hour` mutants per hour of it (per runner, so a sharded sweep's
    figure is one runner's)."""
    per = collections.defaultdict(collections.Counter)
    total = collections.Counter()
    for r in results:
        per[operator_of(r)][r["status"]] += 1
        total[r["status"]] += 1
    ran = [r for r in results if r["status"] != "suppressed"]
    seconds = wall if wall else sum(r.get("seconds", 0) for r in ran)
    return {"operators": {k: dict(v) for k, v in sorted(per.items())},
            "total": dict(total), "run": len(ran),
            "seconds": round(seconds),
            "per_hour": round(len(ran) * 3600 / seconds, 1) if seconds else 0}


def format_summary(summary, not_run=0, not_run_time=0):
    """The summary as a Markdown table."""
    lines = ["| operator | " + " | ".join(STATUSES) + " | total |",
             "|---|" + "---:|" * (len(STATUSES) + 1)]
    rows = list(summary["operators"].items()) + [("**all**",
                                                  summary["total"])]
    for name, counts in rows:
        cells = [str(counts.get(s, 0)) for s in STATUSES]
        lines.append("| %s | %s | %d |" % (
            name, " | ".join(cells), sum(counts.values())))
    lines.append("")
    lines.append("%d mutants ran in %ds: %.1f mutants per hour%s" % (
        summary["run"], summary["seconds"], summary["per_hour"],
        ("; %d not run (over the budget)" % not_run if not_run else "")
        + ("; %d not run (time budget)" % not_run_time
           if not_run_time else "")))
    return "\n".join(lines)


def one_line(text, limit=70):
    text = normalize(text)
    return text if len(text) <= limit else text[:limit - 3] + "..."


def format_survivors(results):
    """Survivors (and errors) grouped by function: a header line per
    function, then `SURVIVOR file:line in function: operator: diff` for each,
    the diff being the tokens that changed."""
    lines = []
    groups = collections.defaultdict(list)
    for r in results:
        if r["status"] == "survived":
            groups[(r["file"], r["function"])].append(r)
    for (path, function), rs in sorted(groups.items()):
        lines.append("== %s %s: %d survivor%s" % (
            path, function, len(rs), "" if len(rs) == 1 else "s"))
        for r in sorted(rs, key=lambda r: r["line"]):
            old, new = operators.token_diff(r["before"], r["replacement"])
            lines.append("SURVIVOR %s:%d in %s: %s: `%s` -> `%s`" % (
                r["file"], r["line"], function, r["op"], one_line(old),
                one_line(new)))
    for r in results:
        if r["status"] == "error":
            lines.append("ERROR mutant %d %s:%d: %s" % (
                r["id"], r["file"], r["line"],
                r.get("tail", "")[-300:].replace("\n", " ")))
    return lines


def run_phases(args, src, phases, timeout):
    """(exit status, output) of the killer phases in order, stopping at the
    first that does not pass; a phase that times out is status 3 (a hang)."""
    rc, tail = 0, ""
    for killers in phases:
        try:
            p = subprocess.run(
                [args.bazel, "test", "--notest_keep_going",
                 "--test_output=errors", "--jobs=2", "--local_test_jobs=2"]
                + killers, cwd=src, capture_output=True, timeout=timeout)
            rc = p.returncode
            tail = (p.stdout + p.stderr).decode("utf-8", "replace")
        except subprocess.TimeoutExpired:
            rc, tail = 3, "run timed out (counted as killed: a hang)"
        if rc != 0:
            break
    return rc, tail


def run(args):
    workspace = os.path.abspath(args.workspace)
    mutants = json.load(open(args.mutants))
    if args.only:
        wanted = {int(x) for x in args.only.split(",")}
        mutants = [m for m in mutants if m["id"] in wanted]
    if args.shard:
        k, n = (int(x) for x in args.shard.split("/"))
        mutants = shard_of(mutants, k, n)
    mutants, suppressed = split_suppressed(
        mutants, load_equivalent(getattr(args, "equivalent", "")))
    if args.sample and args.sample < len(mutants):
        mutants = [m for m in take_turns(mutants, args.seed)][:args.sample]
        mutants.sort(key=lambda m: m["id"])
    scratch = tempfile.mkdtemp(prefix="dcfs-mutate.")
    src = os.path.join(scratch, "src")
    os.makedirs(src)
    copy_tree(workspace, src)
    # Phases, in order; a mutant that survives one goes on to the next. (A
    # size filter such as --config=fast applies to every target of its
    # invocation, so trace validation, a medium test, needs a phase of its
    # own.)
    phases = [ph.split() for ph in args.killers.split(";")]
    results = [dict(m, status="suppressed", seconds=0, killer="")
               for m in suppressed]
    if mutants and getattr(args, "baseline", True):
        # The unmutated tree must pass: otherwise every mutant would be
        # "killed", and the first mutant would pay for the cold build and
        # time out. This run warms the scratch tree's caches.
        print("baseline: the killers on the unmutated tree", file=sys.stderr)
        rc, tail = run_phases(args, src, phases, args.baseline_timeout)
        if rc != 0:
            subprocess.run([args.bazel, "shutdown"], cwd=src,
                           capture_output=True)
            shutil.rmtree(scratch, ignore_errors=True)
            print("the killers fail on the unmutated tree (exit %d), so no "
                  "mutant can be judged:\n%s" % (rc, tail[-1500:]),
                  file=sys.stderr)
            return 2
    started = time.time()
    clock = getattr(args, "clock", time.time)
    not_run_time = 0
    try:
        for i, m in enumerate(mutants, 1):
            deadline = getattr(args, "deadline", None)
            if deadline is not None and clock() >= deadline:
                not_run_time = len(mutants) - i + 1
                print("time budget spent: %d mutants not run" % not_run_time,
                      file=sys.stderr)
                break
            path = os.path.join(src, m["file"])
            original = open(path, "rb").read()
            text = original.decode("latin-1")
            if text[m["start"]:m["end"]] != m["before"]:
                raise SystemExit("mutant %d no longer matches %s "
                                 "(regenerate)" % (m["id"], m["file"]))
            mutated = text[:m["start"]] + m["replacement"] + text[m["end"]:]
            open(path, "wb").write(mutated.encode("latin-1"))
            t = time.time()
            try:
                rc, tail = run_phases(args, src, phases, args.timeout)
            finally:
                open(path, "wb").write(original)
            # Bazel: 0 all passed, 3 a test failed or timed out, 1 a build
            # error (the mutant does not compile), 4 no test ran.
            status = {0: "survived", 3: "killed", 1: "invalid"}.get(
                rc, "error")
            killer = ""
            if status == "killed":
                f = re.findall(r"^(?:FAIL|TIMEOUT):\s+(//\S+)", tail, re.M)
                killer = f[0] if f else ""
            r = dict(m, status=status, seconds=round(time.time() - t, 1),
                     killer=killer)
            if status == "error":
                r["tail"] = tail[-1500:]
            if args.show_output and status == "killed":
                failing = [l for l in tail.splitlines() if re.search(
                    r"\[  FAILED  \]|Failure|Expected|Actual|Value of|FAIL"
                    r"|failed", l)]
                print("\n".join("    | " + l[:200] for l in failing[:12]),
                      file=sys.stderr)
            results.append(r)
            print("[%d/%d] mutant %d %s:%d %s: %s (%.0fs)%s" % (
                i, len(mutants), m["id"], m["file"], m["line"], m["op"],
                status, r["seconds"], " by " + killer if killer else ""),
                file=sys.stderr)
            json.dump(results, open(args.result, "w"), indent=1)
    finally:
        subprocess.run([args.bazel, "shutdown"], cwd=src, capture_output=True)
        shutil.rmtree(scratch, ignore_errors=True)
    wall = time.time() - started
    if not mutants:
        json.dump(results, open(args.result, "w"), indent=1)
    summary = summarize(results, wall if mutants else None)
    table = format_summary(summary, getattr(args, "not_run", 0),
                           not_run_time)
    print(table, file=sys.stderr)
    lines = format_survivors(results)
    print("\n".join(lines))
    if args.survivors_out:
        with open(args.survivors_out, "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + ("\n" if lines else ""))
    if getattr(args, "summary_out", ""):
        with open(args.summary_out, "w", encoding="utf-8") as f:
            f.write(table + "\n")
    return exit_code(results, args.fail_on_survivor)


def report(args):
    """Print the table and the grouped survivors of result files."""
    results = []
    for path in args.results:
        with open(path, encoding="utf-8") as f:
            results.extend(json.load(f))
    print("## Mutation results\n")
    print(format_summary(summarize(results)))
    lines = format_survivors(results)
    print("\n## Survivors by function (%d)\n" % sum(
        1 for l in lines if l.startswith("SURVIVOR")))
    print("```")
    print("\n".join(lines))
    print("```")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)
    workspace = os.environ.get("BUILD_WORKSPACE_DIRECTORY", ".")
    here = os.path.join(workspace, "tools/mutation")
    scope = os.path.join(here, "scope.txt")
    arid = os.path.join(here, "arid.txt")
    equivalent = os.path.join(here, "equivalent.txt")
    killers = "--config=fast //dcfs/...;//dcfs:dir_cache_fs_trace_test"
    g = sub.add_parser("generate")
    g.add_argument("--workspace", default=workspace)
    g.add_argument("--scope", default=scope)
    g.add_argument("--arid", default=arid)
    g.add_argument("--out", required=True)
    g.add_argument("--seed", type=int, default=DEFAULT_SEED)
    g.add_argument("--per-function", type=int, default=DEFAULT_PER_FUNCTION,
                   help="at most this many mutants per function (0: no bound)")
    g.add_argument("--all", action="store_true",
                   help="every candidate, unsampled (to look at them)")
    g.add_argument("--changed", default="", metavar="RANGE",
                   help="only functions whose lines this git range touches")
    r = sub.add_parser("run")
    r.add_argument("--workspace", default=workspace)
    r.add_argument("--mutants", required=True)
    r.add_argument("--result", required=True)
    r.add_argument("--sample", type=int, default=0)
    r.add_argument("--seed", type=int, default=DEFAULT_SEED)
    r.add_argument("--only", default="")
    r.add_argument("--shard", default="", metavar="K/N",
                   help="the K-th of N contiguous id ranges (the scheduled "
                   "job's matrix)")
    r.add_argument("--equivalent", default=equivalent)
    r.add_argument("--fail-on-survivor", action="store_true")
    r.add_argument("--survivors-out", default="")
    r.add_argument("--summary-out", default="")
    r.add_argument("--bazel", default="bazel")
    r.add_argument("--timeout", type=int, default=1800)
    r.add_argument("--baseline-timeout", type=int, default=7200,
                   help="the unmutated run that warms the caches first")
    r.add_argument("--no-baseline", dest="baseline", action="store_false")
    r.add_argument("--show-output", action="store_true",
                   help="print the failing lines of the test that killed a "
                   "mutant")
    r.add_argument("--killers", default=killers)
    c = sub.add_parser("changed", help="per-push mode (see the docstring)")
    c.add_argument("--workspace", default=workspace)
    c.add_argument("--scope", default=scope)
    c.add_argument("--arid", default=arid)
    c.add_argument("--equivalent", default=equivalent)
    c.add_argument("--range", required=True, dest="changed")
    c.add_argument("--max-mutants", type=int, default=DEFAULT_BUDGET,
                   help="the per-push budget")
    c.add_argument("--time-budget", type=int, default=120, metavar="MIN",
                   help="wall-clock minutes (from the start, the cold build "
                   "included) after which the remaining mutants are not run "
                   "(0: none)")
    c.add_argument("--seed", type=int, default=DEFAULT_SEED)
    c.add_argument("--result", required=True)
    c.add_argument("--fail-on-survivor", action="store_true")
    c.add_argument("--survivors-out", default="")
    c.add_argument("--summary-out", default="")
    c.add_argument("--timeout", type=int, default=1800)
    c.add_argument("--baseline-timeout", type=int, default=7200)
    c.add_argument("--no-baseline", dest="baseline", action="store_false")
    c.add_argument("--bazel", default="bazel")
    c.add_argument("--killers", default=killers)
    p = sub.add_parser("report", help="merge result files")
    p.add_argument("results", nargs="+")
    a = ap.parse_args(argv)
    if a.cmd == "generate":
        generate(a)
        return 0
    if a.cmd == "report":
        return report(a)
    if a.cmd == "changed":
        print("seed %d, budget %d mutants, %d minutes" % (
            a.seed, a.max_mutants, a.time_budget), file=sys.stderr)
        if a.time_budget:
            a.deadline = time.time() + 60 * a.time_budget
        found = sample(candidates(a), a.seed)
        live, suppressed = split_suppressed(
            found, load_equivalent(a.equivalent))
        chosen, a.not_run = pick_budget(live, a.max_mutants, a.seed)
        print("%d mutants in the changed functions: %d suppressed, %d to "
              "run, %d not run" % (len(found), len(suppressed), len(chosen),
                                   a.not_run), file=sys.stderr)
        if not found:
            print("no mutants: the range touches no function in scope")
            return 0
        a.out = a.result + ".mutants.json"
        json.dump(number(chosen + suppressed), open(a.out, "w"), indent=1)
        a.mutants, a.only, a.shard = a.out, "", ""
        a.sample = 0
        a.show_output = False
        return run(a)
    return run(a)


if __name__ == "__main__":
    sys.exit(main())
