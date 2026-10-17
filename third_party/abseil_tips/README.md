# Abseil Tips of the Week (pinned, indexed for reviewers)

Why: Abseil's Tips of the Week are adopted as design guidance
(`docs/style.md`, "Precedence, and the Abseil Tips of the Week") and cited by
number in reviews. A pin lets a reviewer without network read the tip behind a
number, keeps reviews reproducible, and makes a citation of a tip that does
not exist fail (`//tools:totw_citations_test`).

## Pin

- **abseil/abseil.github.io, commit `e9e24835cb889fe25251cb9ec6d51b79233e358d`**
  (the `master` tip on 2026-10-17; Bazel repository `@abseil_tips`, an
  `http_archive`).
  - URL: `https://github.com/abseil/abseil.github.io/archive/e9e24835cb889fe25251cb9ec6d51b79233e358d.tar.gz`
  - sha256: `39ef558457e44fa1908194a8f3db432c186779a438dd0a548c7959ceed90a178`
- Licence: **Apache-2.0** (the archive's `LICENSE`, "Apache License Version
  2.0, January 2004"). The tips are Markdown posts in `_posts/`; this
  repository does not copy them, it only builds an index and a concatenation
  from the fetched archive (build outputs, not checked in, never shipped).

The archive has 85 published `*-totw-N.md` posts for 83 tips (TotW 3 and 5
are published in two files each; the later file is taken), plus TotW 132,
which is `published: false` and so left out. The Performance Tips
(`*-fast-N.md`) and the blog posts are not read.

`third_party/abseil_tips/` holds the BUILD overlay (`BUILD.abseil_tips`),
the index rules (`BUILD.bazel`, `make_index.py`) and their tests.

## Reading the tips

`bazel build //third_party/abseil_tips:tips` leaves, in
`bazel-bin/third_party/abseil_tips/`:

| File | Content |
|---|---|
| `index.tsv` | number, title and archive file of each published tip, tab-separated, in number order |
| `tips.md` | every published tip, each under `# TotW #N: title` |

Read tip 122 with `grep -n '^# TotW #122' tips.md`, then the lines after it.

## Checks

- `//third_party/abseil_tips:index_test` fails if the index loses a tip
  `docs/style.md` relies on (42, 103, 122) or is truncated.
- `//third_party/abseil_tips:make_index_test` covers which posts are tips.
- `//tools:totw_citations_test` fails if a TotW number cited in
  `docs/style.md`, `docs/abseil-utilities.md`, `docs/design.md` or the C++
  is not in `index.tsv`. Cite as `TotW #103`, `TotW 103`, `tips/103` or
  `Tip of the Week #103`.

## Updating the pin

1. Take the current `master` tip: `git ls-remote
   https://github.com/abseil/abseil.github.io.git refs/heads/master`.
2. Download `https://github.com/abseil/abseil.github.io/archive/<commit>.tar.gz`
   and `sha256sum` it.
3. Update `urls`, `strip_prefix` and `sha256` of `http_archive(name =
   "abseil_tips")` in `MODULE.bazel`, and the Pin section above.
4. `bazel test //third_party/abseil_tips/... //tools:totw_citations_test`.
   Diff the old and new `tips.md` and read the diff for changed advice; a
   change that alters `docs/style.md`'s premises goes to russ.
5. Bump it with the other pins; there is no timer.
