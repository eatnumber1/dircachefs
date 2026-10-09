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

`--lang tla` (step 12.13) mutates the TLA+ model instead: `generate` and
`changed` read tla_scope.txt (or one `--module`), the operators are
tla_operators.py's, the mutants carry `lang: tla` and `run` judges them by
the `//formal` tests (small, then medium: `--tier small` stops at the first)
with the mutated module in the scratch tree. A mutant survives when every
test still passes (a known_bug test still finds its counterexample); one that
makes TLC fail with an error (a parse or evaluation error, not a violation)
is `invalid`. `generate --max-mutants N` takes N of the sample (seeded, the
operators in turn), `run --time-budget MIN` stops starting mutants after MIN
minutes; `changed --lang tla` mutates the definitions a git range touches.
"""

import argparse
import collections
import hashlib
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

import arid as arid_lib
import operators
import tla_operators as tla

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
STATUSES = ("killed", "survived", "invalid", "suppressed", "error", "flaky")


def fail(message):
    """A tooling error: say so and exit with status 2 (not 1, which is a
    survivor under --fail-on-survivor)."""
    print(message, file=sys.stderr)
    raise SystemExit(2)


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
            fail("scope.txt: cannot read %r" % line)
    return files


def read_tla_scope(path):
    """The module paths of tla_scope.txt (`module PATH` lines)."""
    modules = []
    with open(path, encoding="utf-8") as f:
        lines = f.read().splitlines()
    for line in lines:
        words = line.split("#", 1)[0].split()
        if not words:
            continue
        if len(words) != 2 or words[0] != "module":
            fail("tla_scope.txt: cannot read %r" % line.strip())
        modules.append(words[1])
    return modules


def default_killers(lang, tier):
    """The killer phases of a language: the C++ tests of //dcfs, or the
    `//formal` tests of the small tier and then (unless `--tier small`) the
    medium tier (a size filter applies to a whole invocation, so two)."""
    if lang != "tla":
        return "--config=fast //dcfs/...;//dcfs:dir_cache_fs_trace_test"
    small = "--config=fast //formal/..."
    if tier == "small":
        return small
    return small + ";--config=presubmit //formal/..."


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

    def spelling_in_main(self, loc):
        """Notes a location; True when its tokens are spelled in the main
        file (a macro argument is, a macro's body from a header is not)."""
        if not isinstance(loc, dict):
            return True
        if "spellingLoc" in loc:
            self.note(loc["spellingLoc"])
            in_main = self.in_main()
            self.note(loc["expansionLoc"])
            return in_main
        self.note(loc)
        return self.in_main()

    def visit(self, node, out):
        self.note(node.get("loc"))
        rng = node.get("range")
        here = self.in_main()
        node["_spell"] = True
        if rng:
            begin = self.spelling_in_main(rng.get("begin"))
            end = self.spelling_in_main(rng.get("end"))
            node["_spell"] = begin and end
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
        fn_span = span(fn)
        for site in operators.sites_in(fn, source):
            if fn_span and not (fn_span[0] <= site.start
                                and site.end <= fn_span[1]):
                continue  # not in the function: an offset of a header
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


def operator_rank(seed, m):
    """Where the mutant's operator stands in its function's turn order: a
    hash of the seed, the function and the operator, so no operator is
    always first."""
    text = "%s:%s:%s:%s" % (seed, m["file"], m["function"], m["operator"])
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def take_turns(mutants, seed):
    """The mutants in the seeded order with the operators taking turns (the
    operators in a seeded order of their own), so that a cut keeps several
    operators represented and favors none."""
    groups = collections.defaultdict(list)
    for m in mutants:
        groups[m["operator"]].append(m)
    queues = [sorted(g, key=lambda m: rank(seed, m))
              for g in sorted(groups.values(),
                              key=lambda g: operator_rank(seed, g[0]))]
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


def entry_matches(e, m):
    return (e["file"] == m["file"] and e["function"] == m["function"]
            and e["op"] == m["op"]
            and normalize(e["before"]) == normalize(m["before"])
            and normalize(e["after"]) == normalize(m["replacement"])
            and e.get("nth", m.get("nth", 1)) == m.get("nth", 1))


def suppression(m, entries):
    """The reason a mutant is a known equivalent, or None."""
    for e in entries:
        if entry_matches(e, m):
            return e["reason"]
    return None


def equivalent_warnings(entries, candidates):
    """What to tell the maintainer about entries that no longer pin one
    mutant: one that matches none (its code went) or several (it has no
    `nth`, so it also suppresses any identical mutant added later)."""
    warnings = []
    for e in entries:
        n = sum(1 for m in candidates if entry_matches(e, m))
        where = "%s %s %s `%s`" % (e["file"], e["function"], e["op"],
                                   one_line(e["before"], 40))
        if n == 0:
            warnings.append("equivalent.txt: %s matches no mutant: delete "
                            "it or re-key it" % where)
        elif n > 1:
            warnings.append("equivalent.txt: %s matches %d mutants: add "
                            "`nth`" % (where, n))
    return warnings


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
        fail("bazel could not build the compile prerequisites of %s" % target)


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
    fail("no compile action for %s in %s" % (source, target))


def extended_sources(directory, src, seen=None):
    """The sources of the modules `src` extends (and theirs) found in
    `directory`: what their definitions do to the state tells a guard from
    an effect."""
    seen = set() if seen is None else seen
    out = []
    for name in tla.extends_of(src):
        path = os.path.join(directory, name + ".tla")
        if name in seen or not os.path.exists(path):
            continue
        seen.add(name)
        text = open(path, "rb").read().decode("latin-1")
        out.append(text)
        out.extend(extended_sources(directory, text, seen))
    return out


def tla_candidates(args):
    """Every mutant of the TLA+ modules of the scope (or of `args.module`),
    unsampled; with `args.changed`, only those of the definitions the git
    range touches. No Bazel and no clang: the scanner reads the source."""
    workspace = os.path.abspath(args.workspace)
    modules = read_tla_scope(args.scope)
    for wanted in getattr(args, "module", None) or []:
        if not os.path.exists(os.path.join(workspace, wanted)):
            fail("no module %s in %s" % (wanted, workspace))
    if getattr(args, "module", None):
        modules = list(args.module)
    try:
        arid = tla.TlaArid.parse(open(args.arid, encoding="utf-8").read())
        sets = tla.parse_sets(open(args.sets, encoding="utf-8").read())
    except tla.TlaError as e:
        fail(str(e))
    mutants = []
    for path in modules:
        full = os.path.join(workspace, path)
        if not os.path.exists(full):
            print("%s: not in this tree, skipped" % path, file=sys.stderr)
            continue
        directory = os.path.dirname(full)
        src = open(full, "rb").read().decode("latin-1")
        views = set()
        for name in sorted(os.listdir(directory)):
            if name.endswith(".cfg"):
                with open(os.path.join(directory, name), "rb") as f:
                    views |= tla.view_names_in(f.read().decode("latin-1"))
        hunks = None
        if args.changed:
            hunks = changed_hunks(workspace, args.changed, path)
            if not hunks:
                continue
        try:
            mutants.extend(tla.mutants_of(
                src, path, arid=arid, view_names=views, hunks=hunks,
                sets=sets, extended=extended_sources(directory, src)))
        except tla.TlaError as e:
            fail("%s: %s" % (path, e))
    return mutants


def candidates(args):
    """Every mutant of the scope, unsampled (restricted to the functions
    the git range `args.changed` touches, if any)."""
    if getattr(args, "lang", "cpp") == "tla":
        return tla_candidates(args)
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
            fail("clang failed on %s" % entry["file"])
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


def plan(found, entries, seed, per_function):
    """(live, suppressed): the equivalent mutants split off first, so that
    they take no line or function slot from the others, then the sample."""
    live, suppressed = split_suppressed(found, entries)
    return sample(live, seed, per_function), suppressed


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
    entries = load_equivalent(getattr(args, "equivalent", ""))
    # One equivalent.txt for both languages: an entry is for the mutants of
    # its file's language.
    is_tla = getattr(args, "lang", "cpp") == "tla"
    entries = [e for e in entries if e["file"].endswith(".tla") == is_tla]
    for warning in equivalent_warnings(entries, found):
        print(warning, file=sys.stderr)
    if args.all:
        mutants = found
    else:
        live, suppressed = plan(found, entries, args.seed, args.per_function)
        if getattr(args, "max_mutants", 0):
            live, _ = pick_budget(live, args.max_mutants, args.seed)
        # The suppressed ones go along (without a slot), so that `run` can
        # count them.
        mutants = sorted(live + suppressed,
                         key=lambda m: (m["file"], m["start"], m["end"]))
    number(mutants)
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
            diff = tla.token_diff if r.get("lang") == "tla" \
                else operators.token_diff
            old, new = diff(r["before"], r["replacement"])
            lines.append("SURVIVOR %s:%d in %s: %s: `%s` -> `%s`" % (
                r["file"], r["line"], function, r["op"], one_line(old),
                one_line(new)))
    for r in results:
        if r["status"] == "invalid" and r.get("lang") == "tla":
            lines.append("INVALID %s:%d in %s: %s: %s" % (
                r["file"], r["line"], r["function"], r["op"],
                one_line(r.get("detail", ""), 100)))
        if r["status"] == "flaky":
            lines.append("FLAKY mutant %d %s:%d in %s: %s failed once and "
                         "passed on a rerun" % (r["id"], r["file"], r["line"],
                                                r["function"], r["killer"]))
        if r["status"] == "error":
            lines.append("ERROR mutant %d %s:%d: %s" % (
                r["id"], r["file"], r["line"],
                r.get("tail", "")[-300:].replace("\n", " ")))
    return lines


def install_sigterm():
    """SIGTERM raises SystemExit, so that `finally` removes the scratch
    tree and its Bazel output base (a killed sweep once left 16 GB)."""
    def handler(signum, frame):
        raise SystemExit(128 + signum)
    signal.signal(signal.SIGTERM, handler)


def rmtree_force(path):
    """Remove a tree even if Bazel left read-only directories in it."""
    for root, dirs, _ in os.walk(path):
        for d in dirs:
            try:
                os.chmod(os.path.join(root, d), 0o755)
            except OSError:
                pass
    shutil.rmtree(path, ignore_errors=True)


def cleanup(args, src, startup, scratch):
    """Expunge the scratch tree's Bazel output base and remove the tree."""
    if os.path.isdir(src):
        subprocess.run([args.bazel] + startup + ["clean", "--expunge"],
                       cwd=src, capture_output=True)
    rmtree_force(scratch)


def run_phases(args, src, phases, timeout, startup=()):
    """(exit status, output) of the killer phases in order, stopping at the
    first that does not pass; a phase that times out is status 3 (a hang)."""
    rc, tail = 0, ""
    for killers in phases:
        try:
            p = subprocess.run(
                [args.bazel] + list(startup) + [
                    "test", "--notest_keep_going", "--test_output=errors",
                    "--jobs=2", "--local_test_jobs=2"] + killers,
                cwd=src, capture_output=True, timeout=timeout)
            rc = p.returncode
            tail = (p.stdout + p.stderr).decode("utf-8", "replace")
        except subprocess.TimeoutExpired:
            rc, tail = 3, "run timed out (counted as killed: a hang)"
        if rc != 0:
            break
    return rc, tail


def confirm_kill(args, src, startup, target, timeout):
    """True when `target` fails again with its cached result ignored: a
    test that failed once and passes now is flaky, not a kill."""
    try:
        p = subprocess.run(
            [args.bazel] + list(startup) + [
                "test", "--nocache_test_results", "--test_output=errors",
                "--jobs=2", "--local_test_jobs=2", target],
            cwd=src, capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return True
    return p.returncode != 0


def judge(rc, tail, m):
    """(status, killer) of one mutant from Bazel's exit status: 0 all
    passed (survived), 3 a test failed or timed out (killed), 1 a build
    error: invalid when the compiler's error is in the mutated file,
    otherwise a tooling error (a broken build elsewhere would make every
    mutant look invalid); 4 no test ran and the rest are errors."""
    if rc == 0:
        return "survived", ""
    if rc == 3:
        found = re.findall(r"^(?:FAIL|TIMEOUT):\s+(//\S+)", tail, re.M)
        return "killed", found[0] if found else ""
    if rc == 1 and re.search(re.escape(m["file"]) + r":\d+:\d+: error",
                             tail):
        return "invalid", ""
    return "error", ""


SEMANTIC_STATUS = {0, 11, 12, 13}  # none, deadlock, safety, liveness
VIOLATION = re.compile(
    r"^Error: (Invariant .* is violated|Action property .* is violated"
    r"|Temporal propert\w+ (were|was) violated|Deadlock reached"
    r"|The behavior up to this point|The following behavior constitutes"
    r"|Model checking completed)")
TLC_ERROR_MARKERS = re.compile(
    r"Fatal errors while parsing|Parsing or semantic analysis failed"
    r"|Semantic errors:|Lexical errors")


def first_failure(tail):
    """(target, is_timeout) of the first failed test in Bazel's output."""
    m = re.search(r"^(FAIL|TIMEOUT):\s+(//\S+)", tail, re.M)
    return (m.group(2), m.group(1) == "TIMEOUT") if m else ("", False)


def failure_block(tail, target):
    """The output Bazel printed for one failed test (`--test_output=errors`),
    or the whole tail when it is not found."""
    marker = "Test output for %s:" % target
    at = tail.find(marker) if target else -1
    if at < 0:
        return tail
    rest = tail[at + len(marker):]
    nxt = rest.find("Test output for //")
    return rest if nxt < 0 else rest[:nxt]


def tla_error_line(block):
    """The first line of a failed test's output that says TLC itself failed
    (as opposed to finding a violation), or None."""
    status = re.findall(r"tlc_test: TLC exited with status (\d+)", block)
    lines = block.splitlines()
    first = next((l for l in lines if TLC_ERROR_MARKERS.search(l)
                  or (l.startswith("Error:") and not VIOLATION.match(l))),
                 None)
    if status:
        if int(status[-1]) in SEMANTIC_STATUS:
            return None
        return first or "TLC exited with status %s" % status[-1]
    return first


def judge_tla(rc, tail, m):
    """(status, killer) of one TLA+ mutant from Bazel's exit status: 0
    survived; 3 a test failed or timed out: `killed` (also a known_bug test
    that no longer finds its counterexample), unless TLC itself failed with
    an error rather than a violation (`invalid`: a parse, type or
    evaluation error of the mutant); anything else is a tooling error."""
    if rc == 0:
        return "survived", ""
    if rc != 3:
        return "error", ""
    target, _ = first_failure(tail)
    if tla_error_line(failure_block(tail, target)):
        return "invalid", ""
    return "killed", target


def needs_confirm(lang, tail):
    """Whether a kill is rerun with its cached result ignored: a C++ test
    may be flaky; TLC is deterministic, so a TLA+ kill is rerun only when
    it was a timeout (load can cause one)."""
    if lang != "tla":
        return True
    return first_failure(tail)[1]


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
    install_sigterm()
    lang = mutants[0].get("lang", "cpp") if mutants else (
        suppressed[0].get("lang", "cpp") if suppressed else "cpp")
    judge_one = judge_tla if lang == "tla" else judge
    if getattr(args, "time_budget", 0) and not getattr(args, "deadline",
                                                      None):
        args.deadline = time.time() + 60 * args.time_budget
    # Phases, in order; a mutant that survives one goes on to the next. (A
    # size filter such as --config=fast applies to every target of its
    # invocation, so trace validation, a medium test, needs a phase of its
    # own.)
    killers = args.killers or default_killers(
        lang, getattr(args, "tier", "medium"))
    phases = [ph.split() for ph in killers.split(";")]
    results = [dict(m, status="suppressed", seconds=0, killer="")
               for m in suppressed]
    scratch = tempfile.mkdtemp(prefix="dcfs-mutate.")
    src = os.path.join(scratch, "src")
    # Its own output base, inside the scratch tree: removed with it.
    startup = ["--output_base=" + os.path.join(scratch, "base")]
    started = time.time()
    clock = getattr(args, "clock", time.time)
    not_run_time = 0
    try:
        os.makedirs(src)
        copy_tree(workspace, src)
        if mutants and getattr(args, "baseline", True):
            # The unmutated tree must pass: otherwise every mutant would be
            # "killed", and the first mutant would pay for the cold build
            # and time out. This run warms the scratch tree's caches.
            print("baseline: the killers on the unmutated tree",
                  file=sys.stderr)
            rc, tail = run_phases(args, src, phases, args.baseline_timeout,
                                  startup)
            if rc != 0:
                print("the killers fail on the unmutated tree (exit %d), so "
                      "no mutant can be judged:\n%s" % (rc, tail[-1500:]),
                      file=sys.stderr)
                return 2
        started = time.time()
        for i, m in enumerate(mutants, 1):
            deadline = getattr(args, "deadline", None)
            if deadline is not None and clock() >= deadline:
                not_run_time = len(mutants) - i + 1
                print("time budget spent: %d mutants not run" % not_run_time,
                      file=sys.stderr)
                break
            path = os.path.join(src, m["file"])
            with open(path, "rb") as f:
                original = f.read()
            text = original.decode("latin-1")
            if text[m["start"]:m["end"]] != m["before"]:
                fail("mutant %d no longer matches %s (regenerate)" % (
                    m["id"], m["file"]))
            mutated = text[:m["start"]] + m["replacement"] + text[m["end"]:]
            with open(path, "wb") as f:
                f.write(mutated.encode("latin-1"))
            t = time.time()
            try:
                rc, tail = run_phases(args, src, phases, args.timeout,
                                      startup)
                status, killer = judge_one(rc, tail, m)
                if (status == "killed" and killer
                        and needs_confirm(lang, tail)
                        and not confirm_kill(args, src, startup, killer,
                                             args.timeout)):
                    status = "flaky"
            finally:
                with open(path, "wb") as f:
                    f.write(original)
            r = dict(m, status=status, seconds=round(time.time() - t, 1),
                     killer=killer)
            if status == "error":
                r["tail"] = tail[-1500:]
            if status == "invalid" and lang == "tla":
                r["detail"] = (tla_error_line(failure_block(
                    tail, first_failure(tail)[0])) or "")[:200]
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
            with open(args.result, "w") as f:
                json.dump(results, f, indent=1)
    finally:
        cleanup(args, src, startup, scratch)
    wall = time.time() - started
    if not mutants:
        with open(args.result, "w") as f:
            json.dump(results, f, indent=1)
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
    tla_scope = os.path.join(here, "tla_scope.txt")
    tla_arid = os.path.join(here, "tla_arid.txt")
    tla_sets = os.path.join(here, "tla_sets.txt")
    # None: the language's default (default_killers).
    killers = None

    def add_lang(p):
        p.add_argument("--lang", choices=("cpp", "tla"), default="cpp",
                       help="what to mutate: the C++ of scope.txt (default) "
                       "or the TLA+ modules of tla_scope.txt")
        p.add_argument("--module", action="append", default=[],
                       metavar="PATH", help="with --lang tla: only this "
                       "module (repeatable), instead of tla_scope.txt")
        p.add_argument("--sets", default=tla_sets,
                       help="with --lang tla: the sets of constants that "
                       "stand for each other")

    def add_tier(p):
        p.add_argument("--tier", choices=("small", "medium"),
                       default="medium",
                       help="with --lang tla: the //formal tiers that kill "
                       "a mutant (small: the small tier only)")

    g = sub.add_parser("generate")
    add_lang(g)
    g.add_argument("--workspace", default=workspace)
    g.add_argument("--scope", default=None)
    g.add_argument("--arid", default=None)
    g.add_argument("--max-mutants", type=int, default=0,
                   help="keep this many of the sample (seeded, the operators "
                   "in turn; 0: all)")
    g.add_argument("--equivalent", default=equivalent)
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
    add_tier(r)
    r.add_argument("--time-budget", type=int, default=0, metavar="MIN",
                   help="wall-clock minutes after which no further mutant "
                   "is started (0: none)")
    c = sub.add_parser("changed", help="per-push mode (see the docstring)")
    add_lang(c)
    add_tier(c)
    c.add_argument("--workspace", default=workspace)
    c.add_argument("--scope", default=None)
    c.add_argument("--arid", default=None)
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
    if a.cmd in ("generate", "changed"):
        a.scope = a.scope or (tla_scope if a.lang == "tla" else scope)
        a.arid = a.arid or (tla_arid if a.lang == "tla" else arid)
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
        found = candidates(a)
        live, suppressed = plan(found, load_equivalent(a.equivalent),
                                a.seed, 0)
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
    sys.exit(main(sys.argv[1:]))
