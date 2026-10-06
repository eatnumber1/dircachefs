"""tlc_test: run the TLC model checker on a TLA+ specification as a test.

TLC runs on the host (it needs neither root nor kernel control, so the
QEMU rule in AGENTS.md does not apply), on the hermetic remote JDK pinned
in MODULE.bazel, with the tla2tools.jar pinned there too.

A test either expects TLC to find no error (the default), or, with
`expect_violation`, expects TLC to report exactly that violation: the
known-bug variants in //formal/known_bugs and the documented gaps in
//formal/findings use this, so that the model is shown to be able to find
them. Any other outcome fails the test: a parse error, a different
violation, or no violation at all.
"""

load("@rules_java//java/common:java_common.bzl", "java_common")

def _tlc_test_impl(ctx):
    runtime = ctx.attr._jdk[java_common.JavaRuntimeInfo]
    script = ctx.actions.declare_file(ctx.label.name + ".sh")
    srcs = ctx.files.srcs
    ctx.actions.expand_template(
        template = ctx.file._runner,
        output = script,
        is_executable = True,
        substitutions = {
            "@@JAVA@@": runtime.java_executable_runfiles_path,
            "@@JAR@@": ctx.file._jar.short_path,
            "@@SPEC@@": ctx.file.spec.short_path,
            "@@CONFIG@@": ctx.file.config.short_path,
            "@@SRCS@@": " ".join([f.short_path for f in srcs]),
            "@@EXPECT@@": ctx.attr.expect_violation,
            "@@TLC_ARGS@@": " ".join(ctx.attr.tlc_args),
        },
    )
    runfiles = ctx.runfiles(
        files = [ctx.file._jar, ctx.file.spec, ctx.file.config] + srcs,
        transitive_files = runtime.files,
    )
    return [DefaultInfo(executable = script, runfiles = runfiles)]

tlc_test = rule(
    implementation = _tlc_test_impl,
    test = True,
    doc = "Runs TLC on `spec` with `config`; passes iff the outcome is the expected one.",
    attrs = {
        "spec": attr.label(
            allow_single_file = [".tla"],
            mandatory = True,
            doc = "The root module TLC checks.",
        ),
        "config": attr.label(
            allow_single_file = [".cfg"],
            mandatory = True,
            doc = "The TLC configuration (constants, invariants, properties).",
        ),
        "srcs": attr.label_list(
            allow_files = [".tla"],
            doc = "Other modules `spec` extends or instantiates. All modules " +
                  "are copied into one directory, so basenames must be unique.",
        ),
        "expect_violation": attr.string(
            doc = "Empty: TLC must find no error. Otherwise the exact text " +
                  "TLC prints for the expected violation, e.g. " +
                  "'Invariant CacheNeverWrong is violated'.",
        ),
        "tlc_args": attr.string_list(
            doc = "Extra TLC command-line arguments.",
        ),
        "_jar": attr.label(
            default = "@tla2tools//file",
            allow_single_file = True,
        ),
        "_jdk": attr.label(
            default = "@remotejdk21_linux//:jdk",
            providers = [java_common.JavaRuntimeInfo],
        ),
        "_runner": attr.label(
            default = "//third_party/tlaplus:tlc_test_runner.sh.tpl",
            allow_single_file = True,
        ),
    },
)
