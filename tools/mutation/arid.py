"""Arid nodes: code in which a mutant is noise (plan step 26.5d).

A mutant inside a log statement, a diagnostic string or a testonly hook
changes no behavior a test can observe, so it would only ever survive.
arid.txt lists what to skip, one rule per line with its reason:

  macro NAME...          | reason   the whole statement `NAME(...) << ...;`
  macro-stream NAME...   | reason   only the `<< ...` part after `NAME(cond)`
  function NAME...       | reason   no mutants in functions of these names
  function-regex REGEX   | reason   ... of names matching the regex
  stream-head REGEX      | reason   a `<<` chain whose first operand is a
                                    call of a function matching the regex
                                    (an error builder and its message)

The macro rules are a scan of the text (a macro's expansion has no AST
nodes of its own that say where the macro was), the others read the AST.
"""

import functools
import re

import operators


class AridError(Exception):
    """arid.txt is malformed."""


class Arid:

    def __init__(self, macros=(), macro_streams=(), functions=(),
                 function_regexes=(), stream_heads=()):
        self.macros = {m: r for m, r in macros}
        self.macro_streams = {m: r for m, r in macro_streams}
        self.functions = {f: r for f, r in functions}
        self.function_regexes = [(re.compile(x), r)
                                 for x, r in function_regexes]
        self.stream_heads = [(re.compile(x), r) for x, r in stream_heads]

    @classmethod
    def parse(cls, text):
        rules = {"macro": [], "macro-stream": [], "function": [],
                 "function-regex": [], "stream-head": []}
        for number, line in enumerate(text.splitlines(), 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            rule, bar, reason = line.partition(" | ")
            reason = reason.strip()
            words = rule.split()
            if not bar or not reason:
                raise AridError("arid.txt:%d: a rule needs ` | reason`: %r"
                                % (number, line))
            if len(words) < 2 or words[0] not in rules:
                raise AridError("arid.txt:%d: cannot read %r" % (number, line))
            for arg in words[1:]:
                rules[words[0]].append((arg, reason))
        return cls(rules["macro"], rules["macro-stream"], rules["function"],
                   rules["function-regex"], rules["stream-head"])

    @classmethod
    def load(cls, path):
        with open(path, encoding="utf-8") as f:
            return cls.parse(f.read())

    def function_reason(self, name):
        """Why the function named `name` is arid, or None."""
        if name in self.functions:
            return self.functions[name]
        for rx, reason in self.function_regexes:
            if rx.search(name or ""):
                return reason
        return None

    def spans(self, fn, source):
        """[(start, end, reason)] of the arid ranges in function node `fn`."""
        found = self.text_spans(source, fn)
        found.extend(self.ast_spans(fn, source))
        return found

    def text_spans(self, source, fn):
        sp = operators.span(fn)
        lo, hi = (sp[0], sp[1]) if sp else (0, len(source))
        found = []
        for names, whole in ((self.macros, True), (self.macro_streams, False)):
            if not names:
                continue
            rx = re.compile(r"(?<![\w.:])(%s)\s*\(" % "|".join(
                map(re.escape, sorted(names, key=len, reverse=True))))
            for m in rx.finditer(mask(source), lo, hi):
                line_start = source.rfind("\n", 0, m.start()) + 1
                if source[line_start:m.start()].lstrip().startswith("#"):
                    continue
                close = _matching(source, m.end() - 1)
                if close is None:
                    continue
                end = _statement_end(source, close + 1)
                start = m.start() if whole else close + 1
                found.append((start, end, names[m.group(1)]))
        return found

    def ast_spans(self, fn, source):
        found = []
        if not self.stream_heads:
            return found

        def walk(node):
            if node.get("kind") == "CXXOperatorCallExpr" and node.get(
                    "_main"):
                found_head = self.builder_head(node)
                sp = operators.span(node)
                head = operators.span(found_head[0]) if found_head else None
                if found_head and sp and head and operators.sane(sp, source):
                    # After the head call: the builder's own code (and a
                    # return of the whole expression) stays mutable.
                    found.append((head[1], sp[1], found_head[1]))
            for child in node.get("inner", []):
                walk(child)

        walk(fn)
        return found

    def builder_head(self, node):
        """(head call node, reason) of a `head << a << b` chain whose head
        is arid, or None."""
        while node.get("kind") == "CXXOperatorCallExpr":
            if operators.callee_name(node) != "operator<<" or len(
                    node.get("inner", [])) < 3:
                return None
            node = operators.strip(node["inner"][1])
        if node.get("kind") in operators.CALL_KINDS:
            name = operators.callee_name(node) or ""
            for rx, reason in self.stream_heads:
                if rx.search(name):
                    return node, reason
        return None


@functools.lru_cache(maxsize=8)
def mask(source):
    """The source with the inside of comments, strings and character
    literals blanked (same length, newlines kept), so a scan for macro
    names does not see them."""
    out = []
    i = 0
    while i < len(source):
        j = _skip(source, i)
        if j == i:
            out.append(source[i])
            i += 1
        else:
            out.append("".join(c if c == "\n" else " "
                               for c in source[i:j]))
            i = j
    return "".join(out)


def _skip(source, i):
    """The index after the string, character literal or comment at i, or i."""
    c = source[i]
    if c in "\"'":
        j = i + 1
        while j < len(source) and source[j] != c:
            j += 2 if source[j] == "\\" else 1
        return j + 1
    if source.startswith("//", i):
        j = source.find("\n", i)
        return len(source) if j < 0 else j
    if source.startswith("/*", i):
        j = source.find("*/", i)
        return len(source) if j < 0 else j + 2
    return i


def _matching(source, i):
    """The index of the `)` that closes the `(` at i, or None."""
    depth = 0
    while i < len(source):
        j = _skip(source, i)
        if j != i:
            i = j
            continue
        if source[i] in "([{":
            depth += 1
        elif source[i] in ")]}":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return None


def _statement_end(source, i):
    """The index of the `;` that ends the statement from i (or where the
    enclosing block closes)."""
    depth = 0
    while i < len(source):
        j = _skip(source, i)
        if j != i:
            i = j
            continue
        c = source[i]
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
            if depth < 0:
                return i
        elif c == ";" and depth == 0:
            return i
        i += 1
    return len(source)


def overlaps(site, spans):
    """The reason of the first arid span that contains a Site, or None. (A
    site that merely overlaps one, a return of a whole builder expression,
    or the deletion of a CHECK, stays: it is code around the arid part.)"""
    for start, end, reason in spans:
        if start <= site.start and site.end <= end:
            return reason
    return None
