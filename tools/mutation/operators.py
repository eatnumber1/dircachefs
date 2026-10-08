"""Mutation operators over clang's AST-JSON (plan step 26.5d).

Each operator is a small class with a `name` and a `visit(node, ctx)` that
returns the `Site`s (a replacement of a byte range of the source) it finds
at one AST node. `sites_in` walks the body of a function and asks every
operator at every node, tracking the parents; `mutate.py` drives it. The
operators know nothing about Bazel, files or sampling: they are tested on
the AST of a small C++ fixture (operators_test.py).
"""

import collections
import os
import re

# One mutation: bytes [start, end) of the file become `replacement`.
# `operator` is the class's name (the unit of reporting), `op` says more
# (`negate-if`, `delete-call End`).
Site = collections.namedtuple(
    "Site", "operator op start end replacement before")

FUNCTION_KINDS = {"FunctionDecl", "CXXMethodDecl", "CXXConstructorDecl",
                  "CXXDestructorDecl"}
CALL_KINDS = ("CallExpr", "CXXMemberCallExpr")
PHASE_CALL = re.compile(r"^(Begin|End|Mark)[A-Za-z]*$")
TOKEN = re.compile(
    r"""[A-Za-z_]\w*|\d[\w.']*|"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'"""
    r"""|::|->|&&|\|\||\+\+|--|<<=?|>>=?|[-+*/%&|^!<>=]=?|.""", re.S)


# ---- AST helpers ---------------------------------------------------------

def span(node):
    """(start, end, first token length) of a node in the main file, or None.

    Offsets are bytes. A node that comes out of a macro has a span only when
    its first and last token expand from the same place.
    """
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


def sane(sp, source):
    """True when the span starts at a token of the length clang says.

    The AST dump omits a `file` that did not change, and a node of a header
    can be taken for one of the main file; its offsets then land mid-text.
    """
    start, end, toklen = sp
    if start >= len(source) or source[start].isspace():
        return False
    m = TOKEN.match(source, start)
    return bool(m) and len(m.group(0)) == toklen and end > start


def callee_name(call):
    """The unqualified name a call expression calls, or None."""
    node = call.get("inner", [None])[0]
    while node is not None:
        if node.get("kind") == "DeclRefExpr":
            return node.get("referencedDecl", {}).get("name")
        if node.get("kind") == "MemberExpr":
            return node.get("name")
        inner = node.get("inner")
        node = inner[0] if inner else None
    return None


def strip(node, kinds=("ImplicitCastExpr", "ExprWithCleanups",
                       "CXXBindTemporaryExpr", "MaterializeTemporaryExpr")):
    """The node under any wrappers of `kinds` (first child each time)."""
    while node.get("kind") in kinds and node.get("inner"):
        node = node["inner"][0]
    return node


def qual_type(node):
    return node.get("type", {}).get("qualType", "")


class Context:
    """What an operator may look at besides the node: the file's text, the
    function being mutated, and the chain of the node's parents."""

    def __init__(self, source, fn):
        self.source = source
        self.fn = fn
        self.parents = []

    def text(self, node):
        """(start, end, text) of a node, or None when its span is unusable."""
        sp = span(node)
        if not sp or not node.get("_main") or not sane(sp, self.source):
            return None
        return sp[0], sp[1], self.source[sp[0]:sp[1]]

    def parent(self, skip=("ImplicitCastExpr", "ParenExpr")):
        """The nearest ancestor that is not one of the `skip` wrappers."""
        for p in reversed(self.parents):
            if p.get("kind") not in skip:
                return p
        return None

    def in_lambda(self):
        return any(p.get("kind") == "LambdaExpr" for p in self.parents)


# ---- operators -----------------------------------------------------------

class Operator:
    name = ""

    def visit(self, node, ctx):
        raise NotImplementedError


class Negation(Operator):
    """The condition X of an if, while, for or ?: becomes !(X)."""

    name = "negate"
    KINDS = {"IfStmt": "if", "WhileStmt": "while", "ForStmt": "for",
             "ConditionalOperator": "?:"}

    def visit(self, node, ctx):
        kind = node.get("kind")
        if kind not in self.KINDS:
            return []
        inner = node.get("inner", [])
        idx = 0
        if kind == "IfStmt":
            idx = int(bool(node.get("hasInit"))) + int(
                bool(node.get("hasVar")))
            if node.get("hasVar"):
                idx = None
        elif kind == "WhileStmt":
            idx = None if node.get("hasVar") else 0
        elif kind == "ForStmt":
            # init, condvar, cond, inc, body (empty ones are null nodes).
            idx = 2 if len(inner) >= 3 else None
        if idx is None or idx >= len(inner):
            return []
        cond = inner[idx]
        if not cond.get("kind") or cond.get("kind") == "NullStmt":
            return []
        got = ctx.text(cond)
        if not got:
            return []
        start, end, text = got
        return [Site(self.name, "negate-" + self.KINDS[kind], start, end,
                     "!(" + text + ")", text)]


class _OperatorToken(Operator):
    """Base of the operators that replace a binary operator's token."""

    REPLACEMENTS = {}

    def visit(self, node, ctx):
        if node.get("kind") != "BinaryOperator":
            return []
        op = node.get("opcode")
        if op not in self.REPLACEMENTS or len(node.get("inner", [])) != 2:
            return []
        lhs, rhs = node["inner"]
        left, right = span(lhs), span(rhs)
        if not (left and right and lhs.get("_main") and rhs.get("_main")
                and sane(left, ctx.source) and sane(right, ctx.source)):
            return []
        gap = ctx.source[left[1]:right[0]]
        if not re.fullmatch(r"[\s()]*" + re.escape(op) + r"[\s()]*", gap):
            return []
        start = left[1] + gap.index(op)
        return [Site(self.name, "%s %s" % (self.name, op), start,
                     start + len(op), self.REPLACEMENTS[op], op)]


class RelationalReplacement(_OperatorToken):
    """< <-> <=, > <-> >=, == <-> !=."""

    name = "relational"
    REPLACEMENTS = {"<": "<=", "<=": "<", ">": ">=", ">=": ">",
                    "==": "!=", "!=": "=="}


class LogicalReplacement(_OperatorToken):
    """&& <-> ||."""

    name = "logical"
    REPLACEMENTS = {"&&": "||", "||": "&&"}


class EnumSwap(Operator):
    """The present/absent enumerators exchanged (kFound/kNegative,
    kPresent/kAbsent): the tri-state records of the cache."""

    name = "enum-swap"
    SWAPS = {"kFound": "kNegative", "kNegative": "kFound",
             "kPresent": "kAbsent", "kAbsent": "kPresent"}

    def visit(self, node, ctx):
        if node.get("kind") != "DeclRefExpr":
            return []
        ref = node.get("referencedDecl", {})
        if ref.get("kind") != "EnumConstantDecl" or ref.get(
                "name") not in self.SWAPS:
            return []
        got = ctx.text(node)
        if not got or not got[2].endswith(ref["name"]):
            return []
        start, end, text = got
        new = self.SWAPS[ref["name"]]
        return [Site(self.name, "swap %s/%s" % (ref["name"], new), start, end,
                     text[:-len(ref["name"])] + new, text)]


class ConstantNudge(Operator):
    """An integer literal in a comparison or in + - * / % (also += and the
    like) becomes N+1, N-1 or 0 (a suffix is kept)."""

    name = "constant"
    NUMBER = re.compile(r"(\d[\d']*)([uUlLzZ]*)")
    ARITHMETIC = {"+", "-", "*", "/", "%", "<", "<=", ">", ">=", "==", "!="}

    def visit(self, node, ctx):
        if node.get("kind") != "IntegerLiteral":
            return []
        parent = ctx.parent()
        if parent is None:
            return []
        kind = parent.get("kind")
        if kind == "BinaryOperator":
            if parent.get("opcode") not in self.ARITHMETIC:
                return []
        elif kind != "CompoundAssignOperator":
            return []
        got = ctx.text(node)
        if not got:
            return []
        start, end, text = got
        m = self.NUMBER.fullmatch(text)
        if not m:
            return []
        value, suffix = int(m.group(1).replace("'", "")), m.group(2)
        new = [value + 1] + ([value - 1, 0] if value > 0 else [])
        # N-1 and 0 coincide for N = 1.
        new = list(dict.fromkeys(new))
        return [Site(self.name, "nudge %d" % n, start, end,
                     "%d%s" % (n, suffix), text) for n in new]


class StatementDeletion(Operator):
    """An expression statement (a call, an assignment, ++/--) is deleted:
    its expression becomes (void)0. A call of a Begin*/End*/Mark* function
    that returns absl::Status, used as a value, becomes absl::OkStatus():
    the write-through calls are the point."""

    name = "delete-statement"
    STATEMENT_KINDS = ("CallExpr", "CXXMemberCallExpr", "CXXOperatorCallExpr",
                       "CompoundAssignOperator")
    WRAPPERS = ("ExprWithCleanups", "CXXBindTemporaryExpr",
                "ImplicitCastExpr", "CStyleCastExpr")

    def is_statement(self, node):
        inner = strip(node, self.WRAPPERS)
        kind = inner.get("kind")
        if kind in self.STATEMENT_KINDS:
            return True
        if kind == "BinaryOperator":
            return inner.get("opcode") == "="
        if kind == "UnaryOperator":
            return inner.get("opcode") in ("++", "--")
        return False

    def at_statement(self, ctx):
        """True when only wrappers separate the node from a block."""
        parent = ctx.parent(skip=self.WRAPPERS)
        return parent is not None and parent.get("kind") == "CompoundStmt"

    def visit(self, node, ctx):
        kind = node.get("kind")
        parent = ctx.parents[-1] if ctx.parents else None
        in_block = parent is not None and parent.get("kind") == "CompoundStmt"
        # A void call is a statement wherever it is (`if (c) m->End();`).
        void_call = kind in CALL_KINDS and qual_type(node) == "void"
        if (in_block and self.is_statement(node)) or void_call:
            got = ctx.text(node)
            if not got:
                return []
            start, end, text = got
            name = callee_name(strip(node, self.WRAPPERS))
            label = "delete-call %s" % name if (
                name and strip(node, self.WRAPPERS).get("kind") in CALL_KINDS
            ) else "delete-statement"
            return [Site(self.name, label, start, end, "(void)0", text)]
        if kind in CALL_KINDS and not self.at_statement(ctx):
            name = callee_name(node)
            if (name and PHASE_CALL.match(name)
                    and qual_type(node) == "absl::Status"):
                got = ctx.text(node)
                if got:
                    return [Site(self.name, "delete-call %s" % name, got[0],
                                 got[1], "absl::OkStatus()", got[2])]
        return []


class StatusReturn(Operator):
    """In a function returning absl::Status or absl::StatusOr, a return of
    an error returns OK (Status only: a StatusOr has no OK value to make up)
    and a return of OK or of a value returns an error."""

    name = "status-return"
    RETURNS = re.compile(r"^(?:const\s+)?(absl::Status(?:Or<.*>)?)\s*\(")
    ERROR = 'absl::InternalError("mutant")'

    def visit(self, node, ctx):
        if node.get("kind") != "ReturnStmt" or ctx.in_lambda():
            return []
        m = self.RETURNS.match(qual_type(ctx.fn))
        if not m or not node.get("inner"):
            return []
        value = node["inner"][0]
        got = ctx.text(value)
        if not got:
            return []
        start, end, text = got
        inner = strip(value, ("ImplicitCastExpr", "ExprWithCleanups",
                              "CXXBindTemporaryExpr", "CXXConstructExpr",
                              "MaterializeTemporaryExpr",
                              "CXXFunctionalCastExpr"))
        typ = qual_type(inner)
        ok = inner.get("kind") in CALL_KINDS and callee_name(
            inner) == "OkStatus"
        if m.group(1) == "absl::Status":
            if "Status" not in typ:
                return []
            if ok:
                return [Site(self.name, "ok->error", start, end, self.ERROR,
                             text)]
            return [Site(self.name, "error->ok", start, end,
                         "absl::OkStatus()", text)]
        # StatusOr<T>: a value or OkStatus() becomes an error; a returned
        # status or StatusOr is left alone.
        if "Status" in typ:
            return []
        return [Site(self.name, "value->error", start, end, self.ERROR,
                     text)]


class ArgumentSwap(Operator):
    """Two adjacent arguments of one call, of the same type, exchange."""

    name = "swap-args"
    WRAPPERS = ("ImplicitCastExpr", "ExprWithCleanups",
                "CXXBindTemporaryExpr", "MaterializeTemporaryExpr")

    def visit(self, node, ctx):
        if node.get("kind") not in CALL_KINDS:
            return []
        args = node.get("inner", [])[1:]
        sites = []
        for a, b in zip(args, args[1:]):
            if "CXXDefaultArgExpr" in (a.get("kind"), b.get("kind")):
                continue
            ta, tb = strip(a, self.WRAPPERS), strip(b, self.WRAPPERS)
            if qual_type(ta) != qual_type(tb) or not qual_type(ta):
                continue
            ga, gb = ctx.text(a), ctx.text(b)
            if not ga or not gb or ga[2] == gb[2] or ga[1] > gb[0]:
                continue
            between = ctx.source[ga[1]:gb[0]]
            before = ctx.source[ga[0]:gb[1]]
            sites.append(Site(self.name, "swap-args", ga[0], gb[1],
                              gb[2] + between + ga[2], before))
        return sites


ALL = (RelationalReplacement(), LogicalReplacement(), Negation(),
       EnumSwap(), ConstantNudge(), StatementDeletion(), StatusReturn(),
       ArgumentSwap())


def sites_in(fn, source, operators=ALL):
    """The Sites of every operator in the body of the function node `fn`."""
    ctx = Context(source, fn)
    found = []

    def walk(node):
        if node.get("_main"):
            for op in operators:
                found.extend(op.visit(node, ctx))
        ctx.parents.append(node)
        for child in node.get("inner", []):
            walk(child)
        ctx.parents.pop()

    for child in fn.get("inner", []):
        if child.get("kind") == "CompoundStmt":
            walk(child)
    return found


def token_diff(before, after):
    """`before` and `after` reduced to the tokens that differ."""
    a, b = TOKEN.findall(before), TOKEN.findall(after)
    a = [t for t in a if not t.isspace()]
    b = [t for t in b if not t.isspace()]
    i = 0
    while i < min(len(a), len(b)) and a[i] == b[i]:
        i += 1
    j = 0
    while (j < min(len(a), len(b)) - i
           and a[len(a) - 1 - j] == b[len(b) - 1 - j]):
        j += 1
    return _join(a[i:len(a) - j]), _join(b[i:len(b) - j])


def _join(tokens):
    """Tokens as text: a space only between two words."""
    out = ""
    for t in tokens:
        if out and (out[-1].isalnum() or out[-1] == "_") and (
                t[0].isalnum() or t[0] == "_"):
            out += " "
        out += t
    return out
