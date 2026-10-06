"""exec_file: a tool built once, in the exec configuration, for every config.

The pinned test tools (QEMU, mke2fs, mkfs.xfs, mkfs.btrfs, ...) are built
by rules_foreign_cc, whose configure script embeds the build's `--copt` and
`--linkopt`. In the target configuration, `--config=asan` and
`--config=ubsan` therefore changed the key of every one of those actions
and rebuilt all of them (about 890 s on a cold cache) for a binary that is
not instrumented. The exec configuration takes neither flag (`--copt` does
not apply to it; `--host_copt` would), which is why the kernel, busybox,
bc, the Debian image and the TLC jar have always been independent of the
sanitizer. exec_file makes a tool's file the same on every configuration by
depending on it with `cfg = "exec"`: sh_test's `data` (and the macros'
`data`) have no per-dependency configuration, so the tool's public label is
this forwarding rule and the real target sits behind it.

A side effect that is wanted: the Debian image's `tools = [mke2fs]` already
builds e2fsprogs for the exec configuration, and this rule uses the same
configured target, so e2fsprogs and libarchive are built once, not twice.

Test: //tools:tool_keys_test.
"""

def _exec_file_impl(ctx):
    f = ctx.file.src
    return [DefaultInfo(
        files = depset([f]),
        runfiles = ctx.runfiles(files = [f]),
    )]

exec_file = rule(
    implementation = _exec_file_impl,
    attrs = {
        "src": attr.label(
            allow_single_file = True,
            cfg = "exec",
            mandatory = True,
            doc = "The single file (usually an output group of a " +
                  "configure_make target) to forward.",
        ),
    },
    doc = "Forwards one file of `src`, built in the exec configuration.",
)
