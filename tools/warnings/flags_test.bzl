"""Analysis tests of the warning flags on compile and link command lines.

`.bazelrc` makes every warning in our code an error (`-Werror` for compiles,
`-Wl,--fatal-warnings` for links) and keeps those flags out of external
repositories. Nothing else notices if a BUILD change or a `.bazelrc` edit
drops them: these tests read the command line Bazel builds for a target
under test (an analysis test: no action runs, no root or kernel needed).
"""

load("@bazel_skylib//lib:unittest.bzl", "analysistest", "asserts")

def _outputs(action):
    return ", ".join([f.short_path for f in action.outputs.to_list()])

def _flags_test_impl(ctx):
    env = analysistest.begin(ctx)
    actions = [
        a
        for a in analysistest.target_actions(env)
        if a.mnemonic == ctx.attr.mnemonic
    ]
    asserts.true(
        env,
        len(actions) > 0,
        "no %s action for the target under test" % ctx.attr.mnemonic,
    )
    for action in actions:
        for flag in ctx.attr.present:
            asserts.true(
                env,
                flag in action.argv,
                "%s is missing from the %s command line of %s" % (
                    flag,
                    ctx.attr.mnemonic,
                    _outputs(action),
                ),
            )
        for flag in ctx.attr.absent:
            asserts.false(
                env,
                flag in action.argv,
                "%s must not be on the %s command line of %s" % (
                    flag,
                    ctx.attr.mnemonic,
                    _outputs(action),
                ),
            )
    return analysistest.end(env)

flags_test = analysistest.make(
    _flags_test_impl,
    attrs = {
        "absent": attr.string_list(),
        "mnemonic": attr.string(mandatory = True),
        "present": attr.string_list(),
    },
)
