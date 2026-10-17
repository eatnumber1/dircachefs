# Google style guides (pinned, rendered for reviewers)

Why: `docs/style.md` extends Google's C++, Python, Shell and Markdown guides.
Reviewers and coding agents read the section that applies instead of relying
on memory, and some reviewers (Gemini's sandbox) have no network. A pin keeps
reviews reproducible and offline; a bump shows the guides' changes as a diff.

## Pin

- **google/styleguide, commit `b2a398d17f5ea890c6965b50e6c0c27a8c178f72`**
  (the `gh-pages` tip on 2026-10-17, commit date 2026-10-16, "Automated
  export from internal guides"; Bazel repository `@google_styleguide`, an
  `http_archive`).
  - URL: `https://github.com/google/styleguide/archive/b2a398d17f5ea890c6965b50e6c0c27a8c178f72.tar.gz`
  - sha256: `5ebd83453a1919d9df447e84ea00196c0a72650aab403dbc021b2f9ff5499b40`
- Licence: **CC-BY-3.0** (the archive's `LICENSE` is "Creative Commons Legal
  Code, Attribution 3.0 Unported"; its README: "The style guides in this
  project are licensed under the CC-By 3.0 License"). The rendered copies
  are derived works: the attribution is Google's, from this README and
  `docs/licenses.md`. They are build outputs, not checked in.

`third_party/google_styleguide/` holds the BUILD overlay
(`BUILD.google_styleguide`), the render rules (`BUILD.bazel`), the heading
test and this README.

## Reading the guides

`bazel build //third_party/google_styleguide:guides` renders them with the
pinned pandoc (`//third_party/pandoc`) to GitHub-flavored Markdown in
`bazel-bin/third_party/google_styleguide/`:

| File | Source |
|---|---|
| `cpp.md` | `cppguide.html` (Google C++ Style Guide) |
| `python.md` | `pyguide.md` (Google Python Style Guide) |
| `shell.md` | `shellguide.md` (Google Shell Style Guide) |
| `markdown.md` | `docguide/style.md` (Google Markdown style guide) |

Read the section for the rule in question (`grep -n '^#' cpp.md` lists
them). `headings_test` fails if a guide loses a known section heading, so a
bad render or an upstream restructuring is loud.

## Updating the pin

1. Take the current `gh-pages` tip: `git ls-remote
   https://github.com/google/styleguide.git refs/heads/gh-pages`.
2. Download `https://github.com/google/styleguide/archive/<commit>.tar.gz`
   and `sha256sum` it.
3. Update `urls`, `strip_prefix` and `sha256` of `http_archive(name =
   "google_styleguide")` in `MODULE.bazel`, and the Pin section above.
4. `bazel test //third_party/google_styleguide/...`. Diff the old and new
   rendered guides and read the diff for new or changed rules; a rule that
   changes `docs/style.md`'s premises goes to russ.
5. Bump it with the other pins; there is no timer.
