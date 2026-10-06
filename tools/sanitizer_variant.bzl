"""sanitizer_variant: a target's files as built under `--config=asan`.

Test support for //tools:tool_identity_test: depends on `srcs` through a
transition that sets the flags `build:asan` in .bazelrc sets (the ones that
reach compile and link actions: --copt, --linkopt, --per_file_copt and
--strip; Starlark transitions cannot set --define, which the tools' graphs
do not read), so that one `bazel test` invocation can hold a tool built for
the plain configuration and the same tool as an `--config=asan` build would
get it. Keep in step with .bazelrc.
"""

def _asan_transition_impl(settings, attr):
    return {
        "//command_line_option:copt": settings["//command_line_option:copt"] + [
            "-fsanitize=address",
            "-DADDRESS_SANITIZER",
            "-O1",
            "-g",
            "-fno-omit-frame-pointer",
        ],
        "//command_line_option:linkopt": settings["//command_line_option:linkopt"] + [
            "-fsanitize=address",
        ],
        "//command_line_option:per_file_copt": settings["//command_line_option:per_file_copt"] + [
            "external/(glib|zlib|pcre2)\\+.*@-fno-sanitize=address,-UADDRESS_SANITIZER",
        ],
        "//command_line_option:strip": "never",
    }

_asan_transition = transition(
    implementation = _asan_transition_impl,
    inputs = [
        "//command_line_option:copt",
        "//command_line_option:linkopt",
        "//command_line_option:per_file_copt",
    ],
    outputs = [
        "//command_line_option:copt",
        "//command_line_option:linkopt",
        "//command_line_option:per_file_copt",
        "//command_line_option:strip",
    ],
)

def _sanitizer_variant_impl(ctx):
    # A symlink under this target's own name: runfiles hold one file per
    # logical path, and the sanitizer-flag build of a tool has the same
    # logical path as the plain build, so forwarding the original file would
    # let the plain one shadow it in the test's runfiles and the comparison
    # would see one file twice. The symlink resolves to the file that was
    # actually built for this configuration.
    links = []
    for dep in ctx.attr.srcs:
        for f in dep[DefaultInfo].files.to_list():
            link = ctx.actions.declare_file(ctx.label.name + "/" + f.basename)
            ctx.actions.symlink(output = link, target_file = f)
            links.append(link)
    return [DefaultInfo(
        files = depset(links),
        runfiles = ctx.runfiles(files = links),
    )]

sanitizer_variant = rule(
    implementation = _sanitizer_variant_impl,
    attrs = {
        "srcs": attr.label_list(cfg = _asan_transition, allow_files = True),
        "_allowlist_function_transition": attr.label(
            default = "@bazel_tools//tools/allowlists/function_transition_allowlist",
        ),
    },
    doc = "The files of `srcs`, built with the --config=asan flags.",
)
