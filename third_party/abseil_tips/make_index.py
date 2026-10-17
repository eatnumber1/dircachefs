# SPDX-License-Identifier: MIT
"""Indexes the published Tips of the Week of the pinned abseil.github.io.

argv: the index.tsv to write, the tips.md to write, then the posts
(`_posts/<date>-totw-<N>.md`). A post is a tip when its front matter says
`published: true`; a tip published twice (the 2018 and 2022 files of TotW 3)
is taken from the later file.
"""
import os
import re
import sys

POST_RE = re.compile(r"-totw-(\d+)\.md$")
FRONT_RE = re.compile(r"\A---\n(.*?)\n---\n\n?(.*)\Z", re.S)
TITLE_RE = re.compile(r'^title: "(.*)"$', re.M)
HTML_RE = re.compile(r"<[^>]+>")


def read_tips(paths):
    """{number: (title, path, body)} of the published tips among `paths`."""
    tips = {}
    for path in sorted(paths, key=os.path.basename):
        number = int(POST_RE.search(path).group(1))
        with open(path) as f:
            front, body = FRONT_RE.match(f.read()).groups()
        if "published: true" not in front.splitlines():
            continue
        title = TITLE_RE.search(front).group(1)
        title = title.split(": ", 1)[1]
        tips[number] = (HTML_RE.sub("", title), path, body)
    return tips


def render(tips):
    """The index.tsv and tips.md texts of `tips`, in number order."""
    index = []
    text = []
    for number in sorted(tips):
        title, path, body = tips[number]
        index.append("%d\t%s\t%s\n" % (number, title, path))
        text.append("# TotW #%d: %s\n\n%s" % (number, title, body))
    return "".join(index), "\n".join(text)


def main():
    index, text = render(read_tips(sys.argv[3:]))
    with open(sys.argv[1], "w") as f:
        f.write(index)
    with open(sys.argv[2], "w") as f:
        f.write(text)


if __name__ == "__main__":
    main()
