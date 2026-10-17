# SPDX-License-Identifier: MIT
"""Indexes the published Tips of the Week of the pinned abseil.github.io
(plan step 25.32).

argv: the index.tsv to write, the tips.md to write, then the posts
(`_posts/<date>-totw-<N>.md`). A post is a tip when its front matter says
`published: true`; a tip published twice (the 2018 and 2022 files of TotW 3)
is taken from the later file.
"""
import argparse
import html
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
        with open(path, encoding="utf-8") as f:
            front, body = FRONT_RE.match(f.read()).groups()
        # A post is a tip only when explicitly published (TotW 132 is
        # published: false). Require an exact line match.
        if "published: true" not in front.splitlines():
            continue
        title = TITLE_RE.search(front).group(1)
        # Titles are prefixed with "Tip of the Week #<N>: "; split on the colon
        # to keep only the title itself.
        title = title.split(": ", 1)[1]
        tips[number] = (html.unescape(HTML_RE.sub("", title)), path, body)
    return tips


def render(tips):
    """The index.tsv and tips.md texts of `tips`, in number order."""
    index = []
    text = []
    for number in sorted(tips):
        title, path, body = tips[number]
        archive_path = "_posts/%s" % os.path.basename(path)
        index.append("%d\t%s\t%s\n" % (number, title, archive_path))
        text.append("# TotW #%d: %s\n\n%s" % (number, title, body))
    return "".join(index), "\n".join(text)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("index", help="the index.tsv to write")
    parser.add_argument("tips", help="the tips.md to write")
    parser.add_argument("posts", nargs="+",
                        help="the post markdown files to index")
    args = parser.parse_args(argv)
    index, text = render(read_tips(args.posts))
    with open(args.index, "w", encoding="utf-8") as f:
        f.write(index)
    with open(args.tips, "w", encoding="utf-8") as f:
        f.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
