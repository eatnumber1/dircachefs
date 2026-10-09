"""Mutation operators over TLA+ source (plan step 12.13).

There is no parser: a conjunct-aware token scanner. The source is masked
(comments and the contents of strings blanked, offsets kept), cut into tokens
with their line, column and bracket depth, and into top-level definitions
(a token at column 0 that is followed by `==`). What it understands:

  * junction lists: bulleted `/\\` and `\\/` lists (the first bullet at the
    start of a line or after `IN`, `THEN`, `ELSE`, `:`, `==`, ...; the rest
    at the same column and bracket depth), nested to any depth, with LET
    and IN, `\\A`/`\\E` and IF/THEN/ELSE inside their items;
  * chains: `a /\\ b /\\ c` inline, split at the operator when nothing
    with a looser binding (`\\/`, `=>`, IF, LET, `\\E`, ...) is at the same
    depth and no bulleted list is inside;
  * effects: a definition (or an item) is *effectful* when it mentions a
    primed variable or UNCHANGED, or a definition that does, transitively.
    An effect is never dropped, negated or swapped (TLC cannot evaluate it,
    or "successor state is not completely specified": an INVALID mutant
    that says nothing about the model); a guard is;
  * assignments: `x' = e` and an EXCEPT path `!.f = v` are not comparisons.

What it cannot reach: an infix operator of a chain mixed with a looser one
(`a /\\ b \\/ c`, `\\E x : P /\\ Q`: where the operand ends needs the grammar),
a definition at a column other than 0, operator definitions of the form
`a (+) b ==`, the semantics (a mutant TLC rejects is INVALID, reported
apart), and anything whose meaning is in the cfg (constants, the next-state
relation's fairness).

The operators (the report's unit is `operator`):

  negate         a pure conjunct or disjunct `X` -> `~(X)`; an IF's
                 condition (`negate-if`)
  drop-guard     a pure conjunct of an action -> TRUE (`drop-conjunct`
                 of a predicate); `drop-disjunct`: a disjunct (also of the
                 next-state relation) -> FALSE
  swap-junction  /\\ <-> \\/ : a whole pure bulleted list, or one inline
                 operator
  relational     < <-> <=, > <-> >=, = <-> #
  constant       a number N -> N+-1, TRUE <-> FALSE, a member of a small set
                 (tla_sets.txt: the regimes, a row's states) -> its neighbours
  durability     the second argument of a `Commit(row, sync)` call -> the
                 other level (or FALSE)
  drop-step      an element of a tuple/sequence literal; a `!.pc = "X"` label
                 -> the label the step X itself moves to (skip the step)
  swap-step      two adjacent elements of a tuple/sequence literal

Arid: comments (masked), definitions named by a `VIEW` line of a cfg beside
the module, `def-regex` rules of tla_arid.txt, and source marked
`\\* mutation: arid begin REASON` ... `\\* mutation: arid end` or
`\\* mutation: arid REASON` at the end of a line (that line).
"""

import bisect
import collections
import re

KEYWORDS = {"EXTENDS", "CONSTANT", "CONSTANTS", "VARIABLE", "VARIABLES",
            "ASSUME", "ASSUMPTION", "THEOREM", "INSTANCE", "MODULE",
            "RECURSIVE", "USE", "PROOF", "LOCAL", "LEMMA", "PROPOSITION",
            "COROLLARY", "AXIOM"}
OPEN = {"(": ")", "[": "]", "{": "}", "<<": ">>"}
CLOSE = set(OPEN.values())
JUNCTIONS = ("/\\", "\\/")
# What may precede a first bullet that is not at the start of its line.
BULLET_PREV = {"==", "IN", "THEN", "ELSE", "=>", "<=>", ":", "(", "[", "{",
               "<<", ",", "/\\", "\\/", "~", "->", "|->", "OTHER", "CASE",
               "$$"}
# At depth 0 of a chain: something that binds looser than `/\\` or `\\/`
# (or whose extent only the grammar knows), so the operands are not exact.
LOOSE = {"IF", "THEN", "ELSE", "LET", "IN", "CASE", "\\E", "\\A", "\\EE",
         "\\AA", "CHOOSE", "=>", "<=>", "==", "EXCEPT", "LAMBDA", "~>",
         "\\equiv", "\\implies", "OTHER", "[]", "<>", ":", "|->", "->"}
COMMIT_CALLS = ("Commit",)

TOKEN = re.compile(r'''
    "[^"\n]*"
  | -{4,} | ={4,}
  | /\\ | \\/ | \\[A-Za-z]+
  | <=> | => | == | \|-> | -> | <- | <<  | >> | <= | >= | =< | /= | \.\.\.? 
  | :: | :> | @@ | ~> | \[\] | <>
  | [A-Za-z_][A-Za-z0-9_]*
  | \d+
  | \S
''', re.X)
ORDER = {"<": "<=", "<=": "<", ">": ">=", ">=": ">", "=<": "<",
         "\\leq": "<", "\\geq": ">", "\\le": "<", "\\ge": ">"}
EQUALITY = {"=": "#", "#": "=", "/=": "="}


class TlaError(Exception):
    """A malformed marker, rule file or sets file."""


Tok = collections.namedtuple("Tok", "text start end line col first depth")


# ---- masking and scanning ---------------------------------------------------

def mask(src):
    """`src` with comments blanked (`(* *)` nested, `\\*` to the end of the
    line) and string contents replaced by `_`; same length, same lines."""
    out = list(src)
    i, n = 0, len(src)
    while i < n:
        two = src[i:i + 2]
        if two == "(*":
            depth, j = 1, i + 2
            while j < n and depth:
                if src[j:j + 2] == "(*":
                    depth, j = depth + 1, j + 2
                elif src[j:j + 2] == "*)":
                    depth, j = depth - 1, j + 2
                else:
                    j += 1
            blank(out, i, j)
            i = j
        elif two == "\\*":
            j = src.find("\n", i)
            j = n if j < 0 else j
            blank(out, i, j)
            i = j
        elif src[i] == '"':
            j = i + 1
            while j < n and src[j] != '"' and src[j] != "\n":
                j += 2 if src[j] == "\\" else 1
            for k in range(i + 1, min(j, n)):
                if out[k] != "\n":
                    out[k] = "_"
            i = j + 1
        else:
            i += 1
    return "".join(out)


def blank(out, i, j):
    for k in range(i, j):
        if out[k] != "\n":
            out[k] = " "


def tokenize(masked):
    """Tokens of the masked text with line (1-based), column, whether first
    on its line, and bracket depth (a closer carries the depth after it)."""
    starts = [0] + [m.end() for m in re.finditer("\n", masked)]
    toks, depth, line_of_last = [], 0, 0
    for m in TOKEN.finditer(masked):
        text = m.group()
        line = bisect.bisect_right(starts, m.start())
        col = m.start() - starts[line - 1]
        first = line != line_of_last
        line_of_last = line
        if text in CLOSE:
            depth -= 1
        toks.append(Tok(text, m.start(), m.end(), line, col, first, depth))
        if text in OPEN:
            depth += 1
    return toks


class Definition:

    def __init__(self, name, lo, hi, body):
        self.name, self.lo, self.hi, self.body = name, lo, hi, body


class Module:
    """A scanned module: source, masked text, tokens, definitions."""

    def __init__(self, src, path):
        self.src, self.path = src, path
        self.masked = mask(src)
        self.toks = tokenize(self.masked)
        self.starts = [0] + [m.end() for m in re.finditer("\n", src)]
        self.defs = self.find_definitions()
        self.bullets = set()

    def line_of(self, offset):
        return bisect.bisect_right(self.starts, offset)

    def text(self, a, b):
        return self.src[a:b]

    def find_definitions(self):
        toks, defs = self.toks, []
        heads = [i for i, t in enumerate(toks) if t.col == 0 and t.first]
        for k, i in enumerate(heads):
            hi = heads[k + 1] if k + 1 < len(heads) else len(toks)
            j = i
            if toks[j].text == "LOCAL":
                j += 1
            if j >= hi or toks[j].text in KEYWORDS or not re.match(
                    r"[A-Za-z_]", toks[j].text):
                continue
            name = toks[j].text
            j += 1
            if j < hi and toks[j].text in ("(", "["):
                depth = toks[j].depth
                j += 1
                while j < hi and not (toks[j].text in CLOSE
                                      and toks[j].depth == depth):
                    j += 1
                j += 1
            if j < hi and toks[j].text == "==":
                defs.append(Definition(name, i, hi, j + 1))
        return defs


def words(text):
    return set(re.findall(r"[A-Za-z_][A-Za-z0-9_]*", text))


def effectful_definitions(mod, others=()):
    """Names of the definitions that, transitively, mention a primed
    variable or UNCHANGED, those of the modules it extends (`others`:
    Modules) included."""
    body = {}
    for m in (mod,) + tuple(others):
        for d in m.defs:
            body[d.name] = (m.masked[m.toks[d.body].start:
                                     m.toks[d.hi - 1].end]
                            if d.body < d.hi else "")
    eff = {n for n, t in body.items() if "'" in t or "UNCHANGED" in t}
    refs = {n: words(t) for n, t in body.items()}
    changed = True
    while changed:
        changed = False
        for n, w in refs.items():
            if n not in eff and w & eff:
                eff.add(n)
                changed = True
    return eff


# ---- arid -------------------------------------------------------------------

MARK = re.compile(r"\\\* mutation: arid(?: (begin|end))?(.*)$")


def arid_lines(src):
    """The set of (1-based) lines the source marks arid."""
    lines, begin = set(), None
    for number, text in enumerate(src.split("\n"), 1):
        m = MARK.search(text)
        if not m:
            continue
        kind, reason = m.group(1), m.group(2).strip()
        before = text[:m.start()].strip()
        if kind == "end":
            if begin is None:
                raise TlaError("line %d: `arid end` without `arid begin`"
                               % number)
            lines.update(range(begin, number + 1))
            begin = None
        elif kind == "begin":
            if not reason:
                raise TlaError("line %d: `arid begin` needs a reason"
                               % number)
            if begin is not None:
                raise TlaError("line %d: nested `arid begin`" % number)
            begin = number
        else:
            if not before or not reason:
                raise TlaError("line %d: `\\* mutation: arid REASON` ends a "
                               "line of code and needs a reason" % number)
            lines.add(number)
    if begin is not None:
        raise TlaError("line %d: `arid begin` is never closed" % begin)
    return lines


class TlaArid:
    """tla_arid.txt: `def-regex REGEX | reason` rules."""

    def __init__(self, def_regexes=()):
        self.def_regexes = [(re.compile(x), r) for x, r in def_regexes]

    @classmethod
    def parse(cls, text):
        rules = []
        for number, line in enumerate(text.splitlines(), 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            rule, bar, reason = line.partition(" | ")
            words_ = rule.split()
            if not bar or not reason.strip():
                raise TlaError("tla_arid.txt:%d: a rule needs ` | reason`: "
                               "%r" % (number, line))
            if len(words_) != 2 or words_[0] != "def-regex":
                raise TlaError("tla_arid.txt:%d: cannot read %r"
                               % (number, line))
            rules.append((words_[1], reason.strip()))
        return cls(rules)

    def def_reason(self, name):
        for rx, reason in self.def_regexes:
            if rx.search(name):
                return reason
        return None


def view_names_in(cfg_text):
    """The names a configuration gives to TLC's VIEW."""
    names = set()
    for line in cfg_text.splitlines():
        m = re.match(r"\s*VIEW\s+(\w+)\s*$", line)
        if m:
            names.add(m.group(1))
    return names


def parse_sets(text):
    """tla_sets.txt: `name: member member ...` per line (a member is a
    token: a string, a name); the constants that stand for each other."""
    sets = []
    for number, line in enumerate(text.splitlines(), 1):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        name, colon, rest = line.partition(":")
        members = rest.split()
        if not colon or not name.strip() or len(members) < 2:
            raise TlaError("tla_sets.txt:%d: want `name: a b ...` with two "
                           "or more members: %r" % (number, line))
        sets.append(members)
    return sets


# ---- the sites -------------------------------------------------------------

class Ctx:
    """What the operators of one definition share."""

    def __init__(self, mod, d, eff, sets):
        self.mod, self.d, self.eff, self.sets = mod, d, eff, sets
        self.toks = mod.toks
        self.effectful_def = d.name in eff
        self.out = []

    def add(self, operator, op, start, end, replacement):
        before = self.mod.src[start:end]
        if before != replacement:
            self.out.append((operator, op, start, end, replacement, before))

    def pure(self, a, b):
        """No primed variable, UNCHANGED or effectful definition in the
        source bytes [a, b)."""
        t = self.mod.masked[a:b]
        return "'" not in t and not (words(t) & (self.eff | {"UNCHANGED"}))


def tok_range_text(mod, i, j):
    """Source text of tokens [i, j)."""
    return mod.src[mod.toks[i].start:mod.toks[j - 1].end]


def is_bullet(toks, i):
    t = toks[i]
    return t.first or (i > 0 and toks[i - 1].text in BULLET_PREV)


def find_lists(mod, d):
    """The bulleted lists of a definition: [(op, [(a, b)])] where each
    (a, b) is the byte span of an item and `bullets` the token indices of
    the bullets; also records every bullet in mod.bullets."""
    toks, lists, consumed = mod.toks, [], set()
    for i in range(d.body, d.hi):
        t = toks[i]
        if t.text not in JUNCTIONS or i in consumed or not is_bullet(toks, i):
            continue
        starts, j = [i], i + 1
        while j < d.hi:
            u = toks[j]
            if u.depth < t.depth or (u.text in CLOSE and u.depth < t.depth):
                break
            if u.first and u.col < t.col:
                break
            if u.first and u.col == t.col and u.depth == t.depth:
                if u.text == t.text:
                    starts.append(j)
                    consumed.add(j)
                else:
                    break
            j += 1
        items = []
        for k, s in enumerate(starts):
            nxt = starts[k + 1] if k + 1 < len(starts) else j
            if toks[s].end >= toks[nxt - 1].end:
                continue
            a = toks[s + 1].start if s + 1 < nxt else toks[s].end
            b = toks[nxt - 1].end
            items.append((a, b, s + 1, nxt))
        mod.bullets.update(starts)
        lists.append((t.text, starts, items))
    return lists


def chain_pieces(mod, lo, hi):
    """Split tokens [lo, hi) at its top-level `/\\` (or `\\/`) when the
    operands are exact; returns (op, [(a, b)], [operator token index]) or
    None."""
    toks = mod.toks
    if lo >= hi:
        return None
    base = toks[lo].depth
    ops = {"/\\": [], "\\/": []}
    for i in range(lo, hi):
        t = toks[i]
        if t.depth != base:
            continue
        if t.text in LOOSE:
            return None
        if t.text in JUNCTIONS:
            if i in mod.bullets or i == lo:
                return None
            ops[t.text].append(i)
    if bool(ops["/\\"]) == bool(ops["\\/"]):
        return None
    op = "/\\" if ops["/\\"] else "\\/"
    cuts = ops[op]
    pieces, prev = [], lo
    for c in cuts + [hi]:
        if prev >= c:
            return None
        pieces.append((toks[prev].start, toks[c - 1].end))
        prev = c + 1
    return op, pieces, cuts


def groups_of(mod, d):
    """Every junction group of a definition: (op, pieces, bullet/operator
    token indices, bulleted?, scope span for the purity test)."""
    out = []
    for op, starts, items in find_lists(mod, d):
        if len(items) < 2:
            continue
        out.append((op, [(a, b) for a, b, _, _ in items], starts, True,
                    (items[0][0], items[-1][1])))
    ranges = [(d.body, d.hi)]
    for op, starts, items in find_lists(mod, d):
        ranges.extend((lo, hi) for _, _, lo, hi in items)
    for i in range(d.body, d.hi):
        t = mod.toks[i]
        if t.text in OPEN:
            depth = t.depth
            j = i + 1
            while j < d.hi and not (mod.toks[j].text in CLOSE
                                    and mod.toks[j].depth == depth):
                j += 1
            ranges.append((i + 1, j))
    for lo, hi in ranges:
        found = chain_pieces(mod, lo, hi)
        if found:
            op, pieces, cuts = found
            out.append((op, pieces, cuts, False,
                        (mod.toks[lo].start, mod.toks[hi - 1].end)))
    return out


def item_starts_with_junction(mod, a):
    return mod.masked[a:a + 2] in JUNCTIONS


def group_sites(ctx, groups):
    mod = ctx.mod
    for op, pieces, idx, bulleted, scope in groups:
        pure_all = all(ctx.pure(a, b) for a, b in pieces)
        conj = op == "/\\"
        for a, b in pieces:
            if item_starts_with_junction(mod, a) or not mod.masked[a:b].strip():
                continue
            pure = ctx.pure(a, b)
            if pure:
                ctx.add("negate", "negate-conjunct" if conj
                        else "negate-disjunct", a, b,
                        "~(" + mod.src[a:b] + ")")
            if conj and pure:
                ctx.add("drop-guard" if ctx.effectful_def
                        else "drop-conjunct",
                        "drop-guard" if ctx.effectful_def
                        else "drop-conjunct", a, b, "TRUE")
            if not conj:
                ctx.add("drop-disjunct", "drop-disjunct", a, b, "FALSE")
        other = "\\/" if conj else "/\\"
        if pure_all and ctx.pure(*scope):
            # All the operators of the chain or the list at once: TLA+ does
            # not let `a /\\ b \\/ c` stand without parentheses.
            first, last = mod.toks[idx[0]].start, pieces[-1][1]
            if not bulleted:
                first = pieces[0][0]
            new, pos = [], first
            for s in idx:
                t = mod.toks[s]
                new.append(mod.src[pos:t.start] + other)
                pos = t.end
            new.append(mod.src[pos:last])
            ctx.add("swap-junction",
                    "swap-junction %s -> %s" % (op, other), first, last,
                    "".join(new))


def if_sites(ctx):
    """negate-if: the condition of each IF."""
    mod, toks = ctx.mod, ctx.toks
    for i in range(ctx.d.body, ctx.d.hi):
        if toks[i].text != "IF":
            continue
        depth, j = 0, i + 1
        while j < ctx.d.hi:
            if toks[j].text == "IF":
                depth += 1
            elif toks[j].text == "THEN":
                if depth == 0:
                    break
                depth -= 1
            j += 1
        if j >= ctx.d.hi or j == i + 1:
            continue
        a, b = toks[i + 1].start, toks[j - 1].end
        if item_starts_with_junction(mod, a) or not ctx.pure(a, b):
            continue
        ctx.add("negate", "negate-if", a, b, "~(" + mod.src[a:b] + ")")


def assignment_or_path(ctx, i):
    """True when the `=` at token i is an assignment (`x' = e`) or the `=`
    of an EXCEPT path (`!.f[k] = v`)."""
    toks = ctx.toks
    if toks[i - 1].text == "'":
        return True
    depth = toks[i].depth
    j = i - 1
    while j >= ctx.d.body:
        t = toks[j]
        if t.depth < depth:
            break
        if t.text in CLOSE and t.depth == depth:
            # A group at our level (`[n]` of the path): back to its opener.
            while j > ctx.d.body and not (toks[j].text in OPEN
                                          and toks[j].depth == depth):
                j -= 1
        elif t.depth == depth and t.text == "=":
            return False  # the path's own `=`: this one is in its value
        elif t.depth == depth and t.text in (",", "[", "<<", "(", "{",
                                             "EXCEPT"):
            break
        j -= 1
    return toks[j + 1].text == "!" and toks[j + 1].depth == depth


def token_sites(ctx):
    mod, toks = ctx.mod, ctx.toks
    flat = [w for s in ctx.sets for w in s]
    for i in range(ctx.d.body, ctx.d.hi):
        t = toks[i]
        text = t.text
        if text in ORDER:
            ctx.add("relational", "relational %s -> %s" % (text, ORDER[text]),
                    t.start, t.end, ORDER[text])
        elif text in EQUALITY:
            if text == "=" and assignment_or_path(ctx, i):
                continue
            ctx.add("relational",
                    "relational %s -> %s" % (text, EQUALITY[text]),
                    t.start, t.end, EQUALITY[text])
        elif text.isdigit():
            prev = toks[i - 1].text if i > ctx.d.body else ""
            nxt = toks[i + 1].text if i + 1 < ctx.d.hi else ""
            if ".." in (prev, nxt) or (prev == "[" and nxt == "]"):
                continue
            n = int(text)
            for new in ([n + 1, n - 1] if n else [1]):
                ctx.add("constant", "constant %d -> %d" % (n, new),
                        t.start, t.end, str(new))
        elif text in ("TRUE", "FALSE"):
            new = "FALSE" if text == "TRUE" else "TRUE"
            ctx.add("constant", "constant %s -> %s" % (text, new),
                    t.start, t.end, new)
        else:
            raw = mod.src[t.start:t.end]
            if raw in flat:
                for s in ctx.sets:
                    if raw in s:
                        k = s.index(raw)
                        for new in {s[k - 1], s[(k + 1) % len(s)]} - {raw}:
                            ctx.add("constant",
                                    "constant %s -> %s" % (raw, new),
                                    t.start, t.end, new)


def call_args(ctx, i):
    """For a call whose name token is at i (`Name(` follows): the byte
    spans of its arguments, or None."""
    toks = ctx.toks
    if i + 1 >= ctx.d.hi or toks[i + 1].text != "(":
        return None
    depth = toks[i + 1].depth
    spans, a, j = [], toks[i + 2].start if i + 2 < ctx.d.hi else None, i + 2
    while j < ctx.d.hi:
        t = toks[j]
        if t.text == ")" and t.depth == depth:
            if a is None or j == i + 2:
                return None
            spans.append((a, toks[j - 1].end))
            return spans
        if t.text == "," and t.depth == depth + 1:
            spans.append((a, toks[j - 1].end))
            a = toks[j + 1].start
        j += 1
    return None


def durability_sites(ctx):
    toks = ctx.toks
    if ctx.d.name in COMMIT_CALLS:
        return
    for i in range(ctx.d.body, ctx.d.hi):
        if toks[i].text not in COMMIT_CALLS:
            continue
        args = call_args(ctx, i)
        if not args or len(args) != 2:
            continue
        a, b = args[1]
        old = ctx.mod.src[a:b]
        new = {"TRUE": "FALSE", "FALSE": "TRUE"}.get(old, "FALSE")
        ctx.add("durability", "durability %s -> %s" % (
            " ".join(old.split())[:20], new), a, b, new)


def tuples(ctx):
    """(open token index, [(a, b) element spans]) of every tuple literal
    that is not the argument of UNCHANGED."""
    toks = ctx.toks
    for i in range(ctx.d.body, ctx.d.hi):
        if toks[i].text != "<<":
            continue
        prev = toks[i - 1].text if i > ctx.d.body else ""
        if prev in ("UNCHANGED", "_") or ctx.d.name == "vars":
            continue
        depth = toks[i].depth
        spans, a, j = [], None, i + 1
        while j < ctx.d.hi:
            t = toks[j]
            if a is None:
                a = t.start
            if t.text == ">>" and t.depth == depth:
                if j > i + 1:
                    spans.append((a, toks[j - 1].end))
                break
            if t.text == "," and t.depth == depth + 1:
                spans.append((a, toks[j - 1].end))
                a = None
            j += 1
        else:
            continue
        yield i, spans


def step_sites(ctx, successors):
    mod, toks = ctx.mod, ctx.toks
    for _, spans in tuples(ctx):
        if len(spans) < 2:
            continue
        for k, (a, b) in enumerate(spans):
            if k + 1 < len(spans):
                start, end = a, spans[k + 1][0]
            else:
                start, end = spans[k - 1][1], b
            ctx.add("drop-step", "drop-step", start, end, "")
        for k in range(len(spans) - 1):
            (a, b), (c, e) = spans[k], spans[k + 1]
            if mod.src[a:b] == mod.src[c:e]:
                continue
            ctx.add("swap-step", "swap-step", a, e,
                    mod.src[c:e] + mod.src[b:c] + mod.src[a:b])
    for i in range(ctx.d.body, ctx.d.hi - 4):
        if [t.text for t in toks[i:i + 4]] != ["!", ".", "pc", "="]:
            continue
        lab = toks[i + 4]
        if not lab.text.startswith('"'):
            continue
        name = mod.src[lab.start:lab.end]
        nxt = successors.get(name)
        if nxt and nxt != name:
            ctx.add("drop-step", "drop-step %s -> %s" % (name, nxt),
                    lab.start, lab.end, nxt)


def pc_successors(mod):
    """{label: next label} for the steps `At(p, "X")` that move the pc to
    exactly one other label with `!.pc = "Y"`."""
    out = {}
    toks = mod.toks
    for d in mod.defs:
        label, nxt = None, set()
        for i in range(d.body, d.hi):
            t = toks[i]
            if (t.text == "At" and i + 4 < d.hi and toks[i + 1].text == "("
                    and toks[i + 4].text.startswith('"')):
                label = mod.src[toks[i + 4].start:toks[i + 4].end]
            if (t.text == "!" and i + 4 < d.hi
                    and [x.text for x in toks[i + 1:i + 4]] == [".", "pc", "="]
                    and toks[i + 4].text.startswith('"')):
                nxt.add(mod.src[toks[i + 4].start:toks[i + 4].end])
        if label and len(nxt) == 1:
            out[label] = next(iter(nxt))
    return out


# ---- the mutants of a module -----------------------------------------------

def extends_of(src):
    """The module names in the EXTENDS line of a module."""
    m = re.search(r"\bEXTENDS\b([^=]*?)(?=\n\s*\n|\n[A-Z]+\b)",
                  mask(src))
    return re.findall(r"[A-Za-z_]\w*", m.group(1)) if m else []


def mutants_of(src, path, arid=None, view_names=(), hunks=None, sets=(),
               extended=()):
    """Every mutant of a module's definitions, before sampling: dicts in the
    shape mutate.py uses for C++ (file, function, line, operator, op, start,
    end, replacement, before, key, nth) plus lang "tla". `hunks` restricts
    it to the definitions the changed lines touch; `extended` is the source
    of the modules this one extends (what their definitions do to the
    state is needed to tell a guard from an effect)."""
    mod = Module(src, path)
    arid_set = arid_lines(src)
    eff = effectful_definitions(mod, [Module(e, "") for e in extended])
    succ = pc_successors(mod)
    result, seen = [], set()
    for d in mod.defs:
        if d.name in view_names or (arid and arid.def_reason(d.name)):
            continue
        if d.body >= d.hi:
            continue
        first = mod.toks[d.lo].line
        last = mod.toks[d.hi - 1].line
        if hunks is not None and not any(a <= last and b >= first
                                         for a, b in hunks):
            continue
        ctx = Ctx(mod, d, eff, sets)
        mod.bullets = set()
        groups = groups_of(mod, d)
        group_sites(ctx, groups)
        if_sites(ctx)
        durability_sites(ctx)  # before token_sites: it wins a shared site
        token_sites(ctx)
        step_sites(ctx, succ)
        counts = collections.Counter()
        for operator, op, start, end, replacement, before in sorted(
                ctx.out, key=lambda s: (s[2], s[3], s[4])):
            line, end_line = mod.line_of(start), mod.line_of(end - 1)
            if any(n in arid_set for n in range(line, end_line + 1)):
                continue
            ident = (start, end, replacement)
            if ident in seen:
                continue
            seen.add(ident)
            m = {"file": path, "function": d.name, "line": line,
                 "operator": operator, "op": op, "start": start, "end": end,
                 "replacement": replacement, "before": before, "lang": "tla"}
            m["key"] = " | ".join([path, d.name, op, " ".join(before.split()),
                                   " ".join(replacement.split())])
            counts[m["key"]] += 1
            m["nth"] = counts[m["key"]]
            result.append(m)
    return result


TOKEN_FOR_DIFF = re.compile(
    r'"[^"]*"|\\[A-Za-z]+|\w+|/\\|\\/|<=>|=>|<=|>=|==|<<|>>|\|->|->|\S')


def token_diff(before, after):
    """`before` and `after` reduced to the tokens that differ."""
    a, b = TOKEN_FOR_DIFF.findall(before), TOKEN_FOR_DIFF.findall(after)
    i = 0
    while i < min(len(a), len(b)) and a[i] == b[i]:
        i += 1
    j = 0
    while (j < min(len(a), len(b)) - i
           and a[len(a) - 1 - j] == b[len(b) - 1 - j]):
        j += 1
    return " ".join(a[i:len(a) - j]), " ".join(b[i:len(b) - j])
