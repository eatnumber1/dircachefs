"""clang-tidy and clang-query over our C++ with the build's own flags (25.21).

`style_findings(name, targets)` applies an aspect to each cc_library,
cc_binary and cc_test in `targets` (only those targets, not their deps) and
runs, for every .cc file and for the headers of a target with no .cc file,

- clang-tidy with the checked-in //:.clang-tidy, and
- clang-query with every matcher file of //tools/style_matchers:matchers,

as ordinary Bazel actions: the pinned LLVM (@dcfs_llvm) is the tool, the
inputs are the file, its transitive headers and the toolchain's files, and
the flags are the compile action's, from cc_common (the toolchain's flags,
the --copt flags, the rule's copts, includes and defines). So the result is
sandboxed, hermetic and cached per file, and `bazel test --config=fast
//tools:style_checks_test` reruns only what a change touched.

The outputs are `<package>/_style/<target>/<file>.tidy.txt` and
`.query.txt`; //tools:style_checks.py reads them (see its docstring). The
actions never fail on a finding: the test decides, against the allowlist
tools/style_checks_allow.txt.

Plan step 7.5 asked for an aspect that fails per target; this is the same
aspect with the verdict moved to the test, so that a finding in the
allowlist does not fail the build.
"""

load("@rules_cc//cc:action_names.bzl", "CPP_COMPILE_ACTION_NAME")
load("@rules_cc//cc:find_cc_toolchain.bzl", "find_cpp_toolchain", "use_cc_toolchain")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")

StyleInfo = provider(
    doc = "The clang-tidy and clang-query outputs of a target.",
    fields = {"outputs": "depset of File"},
)

_KINDS = ("cc_library", "cc_binary", "cc_test")

def _flags(ctx, target, cc_toolchain, feature_configuration):
    """The compile action's flags, without the source and output files (the
    tools take the source as an argument of their own)."""
    cc_ctx = target[CcInfo].compilation_context
    user_flags = (
        ctx.fragments.cpp.copts +
        ctx.fragments.cpp.cxxopts +
        [ctx.expand_make_variables("copts", c, {}) for c in ctx.rule.attr.copts]
    )
    variables = cc_common.create_compile_variables(
        feature_configuration = feature_configuration,
        cc_toolchain = cc_toolchain,
        user_compile_flags = user_flags,
        include_directories = cc_ctx.includes,
        quote_include_directories = cc_ctx.quote_includes,
        system_include_directories = cc_ctx.system_includes,
        framework_include_directories = cc_ctx.framework_includes,
        preprocessor_defines = depset(transitive = [cc_ctx.defines, cc_ctx.local_defines]),
    )
    flags = cc_common.get_memory_inefficient_command_line(
        feature_configuration = feature_configuration,
        action_name = CPP_COMPILE_ACTION_NAME,
        variables = variables,
    )
    return flags

def _aspect_impl(target, ctx):
    if ctx.rule.kind not in _KINDS:
        return [StyleInfo(outputs = depset())]
    cc_toolchain = find_cpp_toolchain(ctx)
    feature_configuration = cc_common.configure_features(
        ctx = ctx,
        cc_toolchain = cc_toolchain,
        requested_features = ctx.features,
        unsupported_features = ctx.disabled_features + ["layering_check", "module_maps", "parse_headers"],
    )
    hdrs = getattr(ctx.rule.files, "hdrs", [])  # a cc_binary has none
    srcs = [f for f in ctx.rule.files.srcs if f.extension == "cc"]
    headers = [] if srcs else [f for f in hdrs if f.extension == "h"]
    cc_ctx = target[CcInfo].compilation_context
    inputs = depset(
        ctx.rule.files.srcs + hdrs + ctx.files._matchers + [ctx.file._tidy_config],
        transitive = [cc_ctx.headers, cc_toolchain.all_files],
    )
    tools = depset(
        [ctx.file._tidy, ctx.file._query] + ctx.files._runtime,
    )
    flags = _flags(ctx, target, cc_toolchain, feature_configuration)
    script = []
    for m in ctx.files._matchers:
        script += ["-f", m.path]
    outputs = []
    for source in srcs + headers:
        flags_of_source = ["-x", "c++-header"] + flags if source.extension == "h" else flags
        stem = "_style/%s/%s" % (ctx.label.name, source.short_path)
        query = ctx.actions.declare_file(stem + ".query.txt")

        # clang-query: one parse for every matcher. It exits 0 whatever it
        # matches; a file it cannot parse shows in the output, where
        # style_checks.py reports it.
        ctx.actions.run_shell(
            outputs = [query],
            inputs = inputs,
            tools = tools,
            command = '"$@" > %s 2>&1' % query.path,
            arguments = [ctx.actions.args()
                .add(ctx.file._query)
                .add_all(script)
                .add("--extra-arg=-fcaret-diagnostics-max-lines=1")
                .add("--extra-arg=-fdiagnostics-print-source-range-info")
                .add(source)
                .add("--")
                .add_all(flags_of_source)],
            mnemonic = "StyleQuery",
            progress_message = "clang-query %s" % source.short_path,
        )
        outputs.append(query)
        if ctx.attr.tidy == "yes":
            tidy = ctx.actions.declare_file(stem + ".tidy.txt")

            # clang-tidy exits 1 for a finding (the config makes them errors)
            # and for a file it cannot parse; style_checks.py tells the two
            # apart in the output. Any other status is a crash.
            ctx.actions.run_shell(
                outputs = [tidy],
                inputs = inputs,
                tools = tools,
                command = '"$@" > %s 2>&1; status=$?; [ $status -le 1 ] || exit $status' % tidy.path,
                arguments = [ctx.actions.args()
                    .add(ctx.file._tidy)
                    .add("--quiet")
                    .add("--config-file", ctx.file._tidy_config)
                    .add(source)
                    .add("--")
                    .add_all(flags_of_source)],
                mnemonic = "StyleTidy",
                progress_message = "clang-tidy %s" % source.short_path,
            )
            outputs.append(tidy)
    return [StyleInfo(outputs = depset(outputs))]

_aspect = aspect(
    implementation = _aspect_impl,
    attrs = {
        "tidy": attr.string(values = ["yes", "no"]),
        "_matchers": attr.label(
            default = "//tools/style_matchers:matchers",
            allow_files = True,
        ),
        "_query": attr.label(
            default = "@dcfs_llvm//:bin/clang-query",
            allow_single_file = True,
            cfg = "exec",
        ),
        "_runtime": attr.label_list(
            default = [
                "@dcfs_llvm//:lib/libgcc_s.so.1",
                "@dcfs_llvm//:lib/libstdc++.so.6",
                "@dcfs_llvm//:lib/libz.so.1",
            ],
            allow_files = True,
            cfg = "exec",
        ),
        "_tidy": attr.label(
            default = "@dcfs_llvm//:bin/clang-tidy",
            allow_single_file = True,
            cfg = "exec",
        ),
        "_tidy_config": attr.label(
            default = "//:.clang-tidy",
            allow_single_file = True,
        ),
    },
    fragments = ["cpp"],
    toolchains = use_cc_toolchain(),
)

def _findings_impl(ctx):
    outputs = depset(transitive = [t[StyleInfo].outputs for t in ctx.attr.targets])
    return [DefaultInfo(files = outputs, runfiles = ctx.runfiles(transitive_files = outputs))]

style_findings = rule(
    implementation = _findings_impl,
    attrs = {
        "targets": attr.label_list(
            aspects = [_aspect],
            mandatory = True,
            providers = [CcInfo],
        ),
        "tidy": attr.string(default = "yes", values = ["yes", "no"]),
    },
    doc = "The clang-tidy and clang-query outputs for each of `targets`.",
)

def style_findings_here(name = "style_findings", tidy = "yes"):
    """style_findings over every C++ target declared above this call.

    Put it last in a BUILD file: it reads the targets of the package so far,
    so a target added later in the file is not covered. Left out:

    - a target with a `target_compatible_with`: Bazel refuses to build a
      target that depends on one that is incompatible with the configuration
      (the sanitizer-runtime tests of //dcfs, asan_runtime_test and
      ubsan_runtime_test, are therefore not analysed);
    - a target with the same `srcs` as an earlier one: the fault-injection
      variants of a test build one source against different deps, and
      analysing it again would repeat the findings at the cost of minutes
      (dir_cache_fs_test.cc is 7800 lines, built into four targets).
    """
    seen = {}
    targets = []
    for r in native.existing_rules().values():
        if r["kind"] not in _KINDS or r.get("target_compatible_with"):
            continue
        srcs = str(r.get("srcs"))
        if srcs in seen:
            continue
        seen[srcs] = True
        targets.append(":" + r["name"])
    style_findings(
        name = name,
        targets = targets,
        tidy = tidy,
        testonly = True,
        visibility = ["//tools:__pkg__"],
    )
