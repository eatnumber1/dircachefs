"""qemu_repo: fetches QEMU and, into its own subprojects/dtc, the one
subproject --enable-fdt=internal needs, as two separate sha256-pinned
downloads -- see third_party/qemu/README.md's "The dtc subproject" section
for why this isn't a plain http_archive.

Short version: the QEMU release tarball already ships subprojects/dtc as a
real git checkout at the exact revision its own subprojects/dtc.wrap
names, which would make --enable-fdt=internal need no network access at
all. But Bazel's own repository-fetching tar extraction drops
dotfiles/dotdirs unconditionally, the same way BUILD-file glob() does --
and unlike glob() (where at least the rest of a directory's ordinary files
stay visible), extracting a tarball entry under a path that contains a
`.git` apparently drops the *entire* subprojects/dtc directory, not just
the dotfile (confirmed empirically: every other subprojects/* directory
survives extraction fine; only the one containing a real .git does not).
With no subprojects/dtc directory and no .git, meson treats the
subproject as needing to be fetched, and (correctly, since there's no
network in the build sandbox) fails.

The fix: fetch dtc separately, from the exact commit
subprojects/dtc.wrap already names (same bytes QEMU's own release process
used -- this is not a different pin, just a different transport), placed
directly into subprojects/dtc via download_and_extract's `output=`
argument (a repository-rule-level operation, so it is not subject to
glob()'s or http_archive's dotfile-dropping at all). subprojects/dtc.wrap
is then deleted, which makes meson use that directory as a plain,
unmanaged subproject (not a [wrap-git] with network home)."""

def _qemu_repo_impl(repository_ctx):
    repository_ctx.download_and_extract(
        url = repository_ctx.attr.url,
        sha256 = repository_ctx.attr.sha256,
        stripPrefix = repository_ctx.attr.strip_prefix,
    )
    repository_ctx.download_and_extract(
        url = repository_ctx.attr.dtc_url,
        sha256 = repository_ctx.attr.dtc_sha256,
        stripPrefix = repository_ctx.attr.dtc_strip_prefix,
        output = "subprojects/dtc",
    )

    # See this file's docstring: with no .git in this fetched copy either
    # (download_and_extract unpacks a plain source snapshot, not a git
    # checkout), leaving subprojects/dtc.wrap in place would just hit the
    # same "wrap-git wants to validate/fetch via git" problem this whole
    # rule exists to avoid.
    result = repository_ctx.execute(["rm", "-f", "subprojects/dtc.wrap"])
    if result.return_code != 0:
        fail("qemu_repo: failed to remove subprojects/dtc.wrap: " + result.stderr)

    for patch in repository_ctx.attr.patches:
        repository_ctx.patch(patch, strip = repository_ctx.attr.patch_strip)

    repository_ctx.symlink(repository_ctx.attr.build_file, "BUILD.bazel")

qemu_repo = repository_rule(
    implementation = _qemu_repo_impl,
    attrs = {
        "url": attr.string(mandatory = True, doc = "QEMU release tarball URL."),
        "sha256": attr.string(mandatory = True),
        "strip_prefix": attr.string(mandatory = True),
        "dtc_url": attr.string(mandatory = True, doc = "dtc source archive URL, at the exact commit subprojects/dtc.wrap names."),
        "dtc_sha256": attr.string(mandatory = True),
        "dtc_strip_prefix": attr.string(mandatory = True),
        "patches": attr.label_list(allow_files = True),
        "patch_strip": attr.int(default = 1),
        "build_file": attr.label(mandatory = True, allow_single_file = True),
    },
    doc = "Fetches QEMU plus its dtc subproject (see docstring above) and applies the device-config patch and BUILD file.",
)
