"""Repository rules for packages from an Alpine release branch (step 24.1).

    alpine_index(name = "alpine_index", branch = "v3.24")

    alpine_package(
        name = "alpine_linux_virt",
        index = "@alpine_index//:index.json",
        packages = ["linux-virt"],
    )

The pin is the branch. A stable branch carries one kernel series and one
major.minor of each tool for its life, so an Alpine update inside the branch
(a new kernel build, a tool's patch release) is taken as it comes and never
fails a test or a job. No package version appears in our tree.

`alpine_index` downloads the branch's signed APKINDEX.tar.gz files and
verifies them against the checked-in Alpine keys. `alpine_package` takes the
branch's current version of each named package (and, with `closure`, of
everything it depends on), downloads the apks, checks each one's signature,
the index's checksum of its control segment and its data hash, and unpacks
it under `root/`. `resolved.json` in each repository lists what was taken.
All the work is done by apk.py with the hermetic interpreter from
rules_python (no host tools). Nothing is recorded in MODULE.bazel.lock.

Bazel re-runs a repository rule only when its inputs change, so a
developer keeps what they fetched until `bazel fetch --force
--repo=@alpine_index` (see README.md); a fresh checkout or CI run fetches
the branch's current packages.
"""

_MIRROR = "https://dl-cdn.alpinelinux.org/alpine"

_PYTHON = Label("@python_3_12_x86_64-unknown-linux-gnu//:bin/python3")
_APK = Label("//third_party/alpine:apk.py")
_KEYS = [
    Label("//third_party/alpine:keys/alpine-devel@lists.alpinelinux.org-4a6a0840.rsa.pub"),
    Label("//third_party/alpine:keys/alpine-devel@lists.alpinelinux.org-5261cecb.rsa.pub"),
    Label("//third_party/alpine:keys/alpine-devel@lists.alpinelinux.org-6165ee59.rsa.pub"),
]

def _key_args(rctx):
    args = []
    for key in _KEYS:
        args += ["--key", str(rctx.path(key))]
    return args

def _run(rctx, args, what):
    result = rctx.execute(
        [rctx.path(_PYTHON), rctx.path(_APK)] + args,
        timeout = 900,
    )
    if result.return_code != 0:
        fail("%s: %s" % (what, result.stderr.strip()))
    return result

def _base_url(mirror, branch, repo, arch):
    return "%s/%s/%s/%s" % (mirror, branch, repo, arch)

def _alpine_index_impl(rctx):
    specs = []
    for repo in rctx.attr.repos:
        file = "APKINDEX-%s.tar.gz" % repo
        rctx.download(
            _base_url(rctx.attr.mirror, rctx.attr.branch, repo, rctx.attr.arch) +
            "/APKINDEX.tar.gz",
            file,
        )
        specs.append("%s=%s" % (repo, file))
    _run(
        rctx,
        ["index"] + _key_args(rctx) + [
            "--branch",
            rctx.attr.branch,
            "--out",
            "index.json",
        ] + specs,
        "verifying the Alpine %s index" % rctx.attr.branch,
    )
    rctx.file("BUILD.bazel", 'exports_files(["index.json"])\n')
    rctx.file("MODULE.bazel", 'module(name = "%s")\n' % rctx.name)

alpine_index = repository_rule(
    implementation = _alpine_index_impl,
    attrs = {
        "arch": attr.string(default = "x86_64"),
        "branch": attr.string(
            mandatory = True,
            doc = "The Alpine release branch, e.g. v3.24: the whole pin.",
        ),
        "mirror": attr.string(default = _MIRROR),
        "repos": attr.string_list(
            default = ["main", "community"],
            doc = "The repositories of the branch whose indexes are merged; " +
                  "packages and dependencies resolve across them. All " +
                  "packages of all repos resolve against this one snapshot.",
        ),
    },
    doc = "The verified, merged package index of an Alpine branch.",
)

_BUILD = """\
package(default_visibility = ["//visibility:public"])

# `root/` is the merged, unpacked content of the packages (resolved.json
# lists them); every file in it is a label, e.g. :root/boot/vmlinuz-virt.
exports_files(["resolved.json"] + glob(["root/**"]))

filegroup(
    name = "root",
    srcs = glob(["root/**"], allow_empty = True),
)
"""

def _alpine_package_impl(rctx):
    index = rctx.path(rctx.attr.index)
    args = ["resolve", "--index", index]
    for name in rctx.attr.packages:
        args += ["--name", name]
    if rctx.attr.closure:
        args.append("--closure")
    selected = json.decode(_run(
        rctx,
        args,
        "resolving %s" % ", ".join(rctx.attr.packages),
    ).stdout)

    branch = selected[0]["branch"]
    mirror = rctx.attr.mirror
    pending = []
    for package in selected:
        pending.append((package, rctx.download(
            _base_url(mirror, branch, package["repo"], rctx.attr.arch) +
            "/" + package["file"],
            "apks/" + package["file"],
            block = False,
        )))
    for package, waiter in pending:
        if not waiter.wait().success:
            fail("downloading %s from Alpine %s failed" % (package["file"], branch))

    for package in selected:
        _run(
            rctx,
            ["unpack"] + _key_args(rctx) + [
                "--apk",
                "apks/" + package["file"],
                "--checksum",
                package["checksum"],
                "--out",
                "root",
            ],
            "unpacking %s" % package["file"],
        )
    _run(rctx, ["finish", "--root", "root"], "fixing symlinks")
    for package in selected:
        rctx.delete("apks/" + package["file"])

    resolved = sorted(
        [
            {
                "name": package["name"],
                "origin": package["origin"],
                "version": package["version"],
                "branch": branch,
                "repo": package["repo"],
            }
            for package in selected
        ],
        key = lambda package: package["name"],
    )
    rctx.file("resolved.json", json.encode_indent({
        "branch": branch,
        "packages": resolved,
    }) + "\n")
    rctx.file("BUILD.bazel", _BUILD)
    rctx.file("MODULE.bazel", 'module(name = "%s")\n' % rctx.name)

alpine_package = repository_rule(
    implementation = _alpine_package_impl,
    attrs = {
        "arch": attr.string(default = "x86_64"),
        "closure": attr.bool(
            default = False,
            doc = "Also take every package the named ones depend on.",
        ),
        "index": attr.label(
            mandatory = True,
            allow_single_file = True,
            doc = "The index.json of an alpine_index repository.",
        ),
        "mirror": attr.string(default = _MIRROR),
        "packages": attr.string_list(
            mandatory = True,
            doc = "Binary package names (not origins): the branch's current " +
                  "version of each is taken.",
        ),
    },
    doc = "Alpine packages of the branch's current versions, verified.",
)
