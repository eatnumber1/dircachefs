"""Collects the compiled objects of everything a binary links (step 26.8).

`linked_objects(name, binary)` writes `<name>.txt`, one object path per line
(its runfiles-relative path; external repositories are under `../`),
and carries the objects themselves as runfiles. //tools:banned_symbols_test
reads `nm -u` of each, which attributes a banned symbol to the library that
references it; the linked binary alone cannot (glibc's printf is in it
because libfuse references it).
"""

LinkedObjectsInfo = provider(
    doc = "Compiled objects of a target and everything it depends on.",
    fields = {"objects": "depset of File"},
)

def _aspect_impl(target, ctx):
    direct = []
    if OutputGroupInfo in target and hasattr(target[OutputGroupInfo], "compilation_outputs"):
        direct = target[OutputGroupInfo].compilation_outputs.to_list()
    transitive = []
    for attr in ["deps", "implementation_deps"]:
        for dep in getattr(ctx.rule.attr, attr, []):
            if LinkedObjectsInfo in dep:
                transitive.append(dep[LinkedObjectsInfo].objects)
    return [LinkedObjectsInfo(objects = depset(direct, transitive = transitive))]

_objects_aspect = aspect(
    implementation = _aspect_impl,
    attr_aspects = ["deps", "implementation_deps"],
)

def _linked_objects_impl(ctx):
    objects = depset(transitive = [b[LinkedObjectsInfo].objects for b in ctx.attr.binaries]).to_list()
    manifest = ctx.actions.declare_file(ctx.label.name + ".txt")
    ctx.actions.write(manifest, "".join([
        o.short_path + "\n"
        for o in objects
    ]))
    return [DefaultInfo(
        files = depset([manifest]),
        runfiles = ctx.runfiles(files = objects + [manifest]),
    )]

linked_objects = rule(
    implementation = _linked_objects_impl,
    attrs = {
        "binaries": attr.label_list(aspects = [_objects_aspect], mandatory = True),
    },
    doc = "Manifest and runfiles of the objects linked into `binaries`.",
)
