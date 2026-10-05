"""pkgconfig_shim(name, pkg_name, version, deps): writes a minimal
pkg-config .pc file for one or more plain cc_library dependencies (ones
that were never built with pkg-config support of their own, e.g. a BCR
module's hand-written BUILD overlay), by reading their CcInfo providers
directly and merging the result.

Why this exists: QEMU's meson.build looks up glib with
`dependency('glib-2.0', method: 'pkg-config', required: true)` --
hard-coded to pkg-config, no fallback. rules_foreign_cc's `configure_make`
already turns a `deps = [...]` cc_library into correct CPPFLAGS/LDFLAGS
for the underlying `./configure` invocation (confirmed: glib's and zlib's
`-I`/`-L` directories do appear there), but that mechanism stops at
*search paths* -- it deliberately does not also inject `-lglib-2.0`
itself, since picking the right `-l` name is exactly what the downstream
project's own dependency discovery (here, pkg-config) is supposed to do.
With no .pc file to discover, meson's pkg-config probe reports glib-2.0
not found at all, even though every file it needs is right there on the
search path. This rule closes that one gap: a tiny, generated-at-build-time
.pc file pointing pkg-config at exactly the include/lib directories and
library name(s) Bazel already resolved for `dep`, so `dependency('glib-2.0',
method: 'pkg-config')` succeeds the same way it would against a real
system install.

zlib *also* needs one of these (a `:zlib_pc`, same as `:glib_pc`), despite
first appearances to the contrary: QEMU's meson.build calls
`dependency('zlib', required: true)` with no `method:` restriction, and
meson's built-in zlib-specific probe tries several methods including a
plain compiler-based `-lz` search -- which seemed, at first, to make a
shim unnecessary (the build does get past the zlib `dependency()` call
with none). What that actually meant: meson found the *host's* zlib via
pkg-config (`PKG_CONFIG_PATH` only *adds* a search directory; it doesn't
stop pkg-config from also finding a real system `zlib.pc` in its own
default search path, which every dev/CI machine with `zlib1g-dev` or
equivalent installed has) -- confirmed by the version it reported (the
host's 1.3, not this project's pinned 1.3.2) and by the built binary
depending on `/lib/x86_64-linux-gnu/libz.so.1`. The fix needs two parts,
both in BUILD.qemu: a `:zlib_pc` shim exactly like `:glib_pc`, *and*
`PKG_CONFIG_LIBDIR` (which replaces pkg-config's default search path
instead of adding to it, unlike `PKG_CONFIG_PATH`) set to cover only this
build's own shim directories -- otherwise pkg-config would still find the
host's real zlib.pc (and, for that matter, a host glib-2.0.pc too, on any
machine that has one) ahead of or alongside this project's.

Fully static, on purpose: the `Libs:` field this rule writes is a list of
literal paths to each dep's *static* archive, never a bare `-lfoo`
search-by-name flag. Found the hard way, from the built
`qemu-system-x86_64`'s own `readelf -d`: pcre2 and zlib (unlike glib
itself) each have *both* a static (`.a`) and a shared (`.so`) artifact
built by Bazel in the very same directory (confirmed via `bazel cquery
... --output=starlark` on their CcInfo directly), and a bare `-lpcre2`/
`-lz` lets the linker pick -- it prefers the shared one whenever both are
present, regardless of which this rule "meant". That produced a binary
whose `NEEDED` entries included the *host's* `/lib/x86_64-linux-gnu/
libz.so.1` (QEMU's `dependency('zlib', ...)` has no `method:` restriction
and meson's built-in zlib probe also resolves via a plain system library
search when nothing more specific says otherwise) and an *absolute path
straight into this one build's own Bazel output_base* baked into pcre2's
`NEEDED` entry and `RUNPATH` -- which would break in any other checkout,
in CI, from a shared cache hit, or the moment that output_base is
cleaned. A literal archive path in `Libs:` can't have either problem: ld
links exactly that file, full stop, and if a dep genuinely has no static
artifact at all this rule fails loudly (`pkgconfig_shim ...: ... has no
static library`) rather than silently falling back to whatever `-lfoo`
would have found.

This one shim actually merges two deps, @glib//glib and @glib//gmodule:
QEMU's meson.build never calls `dependency('gmodule...')` at all in this
configuration (modules and plugins are both disabled, see meson.build's
`if enable_modules / elif get_option('plugins') / else: gmodule =
not_found` -- a not_found dependency contributes nothing and errors on
nothing), but `include/qemu/transactions.h` unconditionally has `#include
<gmodule.h>` regardless of whether anything downstream actually links
GModule. A real system install's glib-2.0.pc does not carry gmodule's
include path (upstream keeps them as two separate pkg-config packages),
but since nothing here ever consults a gmodule*.pc file either, the
simplest correct fix for this one build is to fold gmodule's header
directory into the same glib-2.0.pc -- it only ever affects
`#include <gmodule.h>` resolving, not what (if anything) actually gets
linked for it.
"""

load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")

def _pkgconfig_shim_impl(ctx):
    includes = []
    seen_includes = {}
    lib_files = []
    seen_lib_files = {}

    for dep in ctx.attr.deps:
        cc_info = dep[CcInfo]
        compilation_context = cc_info.compilation_context
        linking_context = cc_info.linking_context

        for d in (
            compilation_context.quote_includes.to_list() +
            compilation_context.includes.to_list() +
            compilation_context.system_includes.to_list()
        ):
            if d not in seen_includes:
                seen_includes[d] = True
                includes.append(d)

        for linker_input in linking_context.linker_inputs.to_list():
            for lib in linker_input.libraries:
                # Static only, deliberately -- see this file's docstring's
                # "Fully static, on purpose" section. A bare "-lfoo" was
                # tried first and is wrong whenever a dep has *both* a
                # static and a shared artifact in the same directory
                # (pcre2 and zlib both do, confirmed by inspecting their
                # CcInfo directly): the linker prefers .so over .a given
                # only a name, regardless of which one this rule "meant".
                # A real path to the .a file is unambiguous.
                f = lib.static_library or lib.pic_static_library
                if not f:
                    fail((
                        "pkgconfig_shim {name}: {dep} has no static " +
                        "library (only a shared one), so it can't be " +
                        "linked hermetically into a pkg-config-discovered " +
                        "dependency. Build it with linkstatic, or give " +
                        "this rule a different target for it."
                    ).format(name = ctx.label.name, dep = dep.label))
                if f.path not in seen_lib_files:
                    seen_lib_files[f.path] = True
                    lib_files.append(f)

    # configure_make's `env` wants a *directory* for PKG_CONFIG_PATH (not a
    # single file), so the .pc file is written directly into its own
    # declared output directory.
    pc_dir = ctx.actions.declare_directory(ctx.label.name)

    # Every File.path below (both pc_dir's own and every include/lib
    # dependency's) is execroot-relative -- "external/<repo>/..." for a
    # source file, "bazel-out/<cpu>-<mode>/bin/external/<repo>/..." for a
    # generated one. pc_dir.path is itself such a string (it's a generated
    # directory), so counting its path segments gives exactly how many
    # "../" hops get from the .pc file's own directory back to the
    # execroot -- from there, every dependency path (already
    # execroot-relative) resolves correctly, regardless of which kind it
    # is. This avoids baking in any sandbox-instance-specific absolute
    # path (each Bazel action gets its own private sandbox root, so an
    # absolute path captured in one action's sandbox would not exist in
    # another's -- see this file's docstring and third_party/qemu/README.md).
    # pkg-config's standard `${pcfiledir}` variable supplies the one
    # thing that *is* stable: the real, final location of the .pc file
    # itself at the time something actually reads it.
    back_to_root = "../" * len(pc_dir.path.split("/"))

    def pcfiledir_rel(path):
        return "${pcfiledir}/" + back_to_root + path

    cflags = " ".join(["-I" + pcfiledir_rel(d) for d in includes])

    # Each entry is the literal path to a .a file, not "-lfoo": gcc/ld
    # accept a path to a static archive as an ordinary positional link
    # input, and unlike "-lfoo" (which needs a search dir and then picks
    # *some* libfoo.{a,so} out of it), a path can't accidentally resolve
    # to a different file than the one this rule actually means.
    libs = " ".join([pcfiledir_rel(f.path) for f in lib_files])

    dep_labels = ", ".join([str(dep.label) for dep in ctx.attr.deps])
    pc_content = """\
Name: {name}
Description: generated pkg-config shim for {dep_labels} (see third_party/qemu/README.md's "Build system" / pkgconfig_shim.bzl)
Version: {version}
Cflags: {cflags}
Libs: {libs}
""".format(
        name = ctx.attr.pkg_name,
        dep_labels = dep_labels,
        version = ctx.attr.version,
        cflags = cflags,
        libs = libs,
    )

    pc_file = ctx.actions.declare_file(ctx.label.name + ".pc_staging/" + ctx.attr.pkg_name + ".pc")
    ctx.actions.write(pc_file, pc_content)

    # declare_directory's TreeArtifact can't itself be a declare_file
    # target (Bazel treats it as one opaque output), so the actual
    # content is written to a plain file first and copied in here.
    ctx.actions.run_shell(
        outputs = [pc_dir],
        inputs = [pc_file],
        command = "mkdir -p {out} && cp {pc} {out}/{name}.pc".format(
            out = pc_dir.path,
            pc = pc_file.path,
            name = ctx.attr.pkg_name,
        ),
        mnemonic = "PkgconfigShim",
        progress_message = "Generating pkg-config shim for %s" % ctx.attr.pkg_name,
    )

    return [DefaultInfo(files = depset([pc_dir]))]

pkgconfig_shim = rule(
    implementation = _pkgconfig_shim_impl,
    attrs = {
        "deps": attr.label_list(mandatory = True, providers = [CcInfo]),
        "pkg_name": attr.string(mandatory = True),
        "version": attr.string(mandatory = True),
    },
    doc = "Writes <pkg_name>.pc (in its own output directory) by merging the CcInfo of one or more cc_library deps.",
)
