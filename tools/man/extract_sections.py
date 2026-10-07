#!/usr/bin/env python3
"""Builds the markdown source of the dcfs(8) man page from README.md.

Usage: extract_sections.py README.md version.h SECTION [SECTION...] > dcfs.md

Each SECTION is the title of a `##` or `###` heading of the README. The
"Usage" section supplies the SYNOPSIS (its first code block); the others
become man sections (heading promoted to level 1 and upper-cased). A
missing section is an error, so renaming one in the README breaks the build
instead of silently dropping it from the page.
"""
import re
import sys

NAME = "dcfs - write-through FUSE metadata cache in SQLite"
SEE_ALSO = "`mount(8)`, `fuse(4)`"


def parse_sections(lines):
    """Returns {title: (level, body lines)} for ## and ### headings."""
    sections = {}
    current = None
    fenced = False
    for line in lines:
        if line.startswith("```"):
            fenced = not fenced
        m = None if fenced else re.match(r"^(#{1,6}) (.*)$", line)
        if m:
            level, title = len(m.group(1)), m.group(2).strip()
            if level <= 3:
                current = (level, [])
                sections[title] = current
                continue
        if current is not None:
            current[1].append(line)
    return sections


def promote(body, level):
    """Demotes nested headings so the section heading is level 1."""
    out = []
    fenced = False
    for line in body:
        if line.startswith("```"):
            fenced = not fenced
        m = None if fenced else re.match(r"^(#{4,6}) (.*)$", line)
        if m:
            line = "#" * (len(m.group(1)) - (level - 1)) + " " + m.group(2)
        out.append(line)
    return convert_tables("\n".join(out).strip("\n"))


def split_row(line):
    return [c.strip() for c in line.strip().strip("|").split("|")]


def table_to_deflist(rows):
    """Man pages document options as a definition list, not a table."""
    header, rows = rows[0], rows[2:]
    if len(header) > 3:
        sys.exit("extract_sections: table with more than three columns: " +
                 " | ".join(header))
    out = []
    for cells in rows:
        term = cells[0]
        term = "**%s**" % term.strip("`") if term.startswith("`--") \
            else "**%s**" % term
        if len(cells) == 3:
            default, meaning = cells[1], cells[2]
            if default == "(required)":
                term += " (required)"
            elif default:
                term += " (default: %s)" % default
        else:
            meaning = cells[-1]
        out += [term, ":   " + meaning, ""]
    return out


def convert_tables(text):
    out, table, fenced = [], [], False

    def flush():
        if table:
            out.extend(table_to_deflist([split_row(l) for l in table]))
            table.clear()

    for line in text.split("\n"):
        if line.startswith("```"):
            fenced = not fenced
        if not fenced and line.startswith("|"):
            table.append(line)
            continue
        flush()
        out.append(line)
    flush()
    return "\n".join(out)


def synopsis(body):
    m = re.search(r"```\n(.*?)\n```", "\n".join(body), re.S)
    if not m:
        sys.exit("extract_sections: no code block in the Usage section")
    return m.group(1)


def main(argv):
    if len(argv) < 4:
        sys.exit(__doc__)
    readme, header, titles = argv[1], argv[2], argv[3:]
    with open(header) as f:
        m = re.search(r'kVersion\[\] = "([^"]*)"', f.read())
    if not m:
        sys.exit("extract_sections: kVersion not found in " + header)
    version = m.group(1)
    with open(readme) as f:
        sections = parse_sections(f.read().split("\n"))
    missing = [t for t in titles if t not in sections]
    if missing:
        sys.exit("extract_sections: README.md has no section(s): " +
                 ", ".join(missing))

    out = ["% dcfs(8) dcfs " + version + " | System Administration", "",
           "# NAME", "", NAME, ""]
    for title in titles:
        level, body = sections[title]
        if title == "Usage":
            out += ["# SYNOPSIS", "", "```", synopsis(body), "```", ""]
        else:
            out += ["# " + title.upper(), "", promote(body, level), ""]
    out += ["# SEE ALSO", "", SEE_ALSO, ""]
    sys.stdout.write("\n".join(out))


main(sys.argv)
