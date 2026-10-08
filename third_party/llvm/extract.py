"""Unpacks the archives of the pinned C/C++ toolchain (plan step 7.1b).

    extract.py llvm ARCHIVE STRIP_PREFIX OUT_DIR
    extract.py deb DATA_TAR_XZ OUT_DIR [--strip-prefix DIR]

`llvm` unpacks LLVM's release tarball without what no dcfs build step uses.
The Linux x86_64 release is 1.9 GB packed and 12 GB unpacked, most of it the
static libraries of LLVM, clang, MLIR and flang, lldb and the tools of flang,
MLIR and the clang tooling libraries: unpacking all of it took 12 to 17
minutes of every pin change. The members matched by SKIP are not written, and
neither is a link whose target is one of them (`bin/llvm-ml64 -> llvm-ml`
would dangle); a chain of links that still dangles afterwards is an error. Everything the toolchain uses is kept: clang, lld, the llvm-*
binutils, llvm-profdata and llvm-cov, clang-tidy, clang-format and
clang-query, the headers, libc++, libunwind and compiler-rt;
//third_party/llvm:extract_test checks the list against what toolchains_llvm
refers to. The repository rule watches this file, so editing the skip list
refetches the repository.

`deb` unpacks the data.tar.xz of a Debian package. Debian's absolute symlinks
(`/lib64/ld-linux-x86-64.so.2 -> /lib/x86_64-linux-gnu/ld-linux-x86-64.so.2`)
are written as relative links, so that the tree can be moved (the repository
contents cache, vendoring, remote execution). With --strip-prefix only the
files under that directory are unpacked, directly into OUT_DIR.

Both unpack with the `data` extraction filter (no absolute paths, no links out
of OUT_DIR) and fail when nothing is written.
"""

import argparse
import os
import re
import sys
import tarfile

SKIP = re.compile(
    "|".join([
        # Static libraries of LLVM, clang, MLIR, flang, lld, Polly (the
        # runtime libraries are in lib/<triple>/ and lib/clang/).
        r"lib/[^/]+\.a",
        r"lib/(cmake|python[0-9.]*|objects-RELEASE|libear|libscanbuild)"
        r"(/.*)?",
        r"lib/(liblldb[^/]*|libclang\.so[^/]*|libLTO\.so[^/]*|LLVMPolly\.so"
        r"|libRemarks\.so[^/]*|libmlir_[^/]*)",
        # Headers of the compiler libraries (libc++'s are include/c++).
        r"include/(llvm|llvm-c|clang|clang-c|mlir|mlir-c|flang|lld|lldb"
        r"|polly|clang-tidy|clang-include-fixer)(/.*)?",
        # Tools nothing here runs.
        r"bin/(mlir-[^/]*|flang[^/]*|f18[^/]*|fir-[^/]*|bbc|tco|lldb[^/]*"
        r"|clang-repl|clang-check|clang-doc|clang-change-namespace"
        r"|clang-move|clang-include-fixer|clang-reorder-fields"
        r"|clang-refactor|clang-extdef-mapping|clang-installapi"
        r"|clang-nvlink-wrapper|clang-sycl-linker|clang-linker-wrapper"
        r"|find-all-symbols|c-index-test|modularize|pp-trace|bugpoint"
        r"|opt|llc|lli|llvm-lto[0-9]*|llvm-reduce|llvm-ir2vec"
        r"|llvm-dwarfutil|llvm-c-test|llvm-exegesis|llvm-bolt[^/]*"
        r"|llvm-jitlink|llvm-split|llvm-gsymutil|llvm-cfi-verify"
        r"|llvm-profgen|llvm-mca|llvm-rtdyld|llvm-debuginfo-analyzer"
        r"|llvm-ml|sancov|dsymutil|perf2bolt|merge-fdata)",
    ]))


class ExtractError(Exception):
    pass


def _normal(name):
    return os.path.normpath(name).lstrip("/") if name else ""


def _link_target(name, linkname):
    """The archive path a symlink at `name` with target `linkname` names."""
    return _normal(os.path.join(os.path.dirname(name), linkname))


def _dangling_links(out):
    """The symlinks under `out` whose target does not exist."""
    found = []
    for root, dirs, files in os.walk(out):
        for entry in dirs + files:
            path = os.path.join(root, entry)
            if os.path.islink(path) and not os.path.exists(path):
                found.append(os.path.relpath(path, out))
    return sorted(found)


def extract_llvm(archive, prefix, out):
    prefix = prefix.rstrip("/") + "/"
    kept = skipped = 0
    with tarfile.open(archive, mode="r|xz") as tar:
        for member in tar:
            if not member.name.startswith(prefix):
                continue
            member.name = _normal(member.name[len(prefix):])
            if not member.name:
                continue
            if member.islnk():
                if not member.linkname.startswith(prefix):
                    raise ExtractError(
                        f"{archive}: {member.name} is a hard link to"
                        f" {member.linkname}, outside {prefix!r}")
                target = _normal(member.linkname[len(prefix):])
                member.linkname = target
            elif member.issym():
                target = _link_target(member.name, member.linkname)
            else:
                target = None
            if SKIP.fullmatch(member.name) or (
                    target is not None and SKIP.fullmatch(target)):
                skipped += member.size
                continue
            kept += 1
            tar.extract(member, out, filter="data")
    if not kept:
        raise ExtractError(
            f"{archive}: no member under {prefix!r} was written (wrong strip"
            " prefix?)")
    # A link to a link to a skipped file passes the check above.
    dangling = _dangling_links(out)
    if dangling:
        raise ExtractError(
            "links to files the skip list leaves out: " + ", ".join(dangling))
    return kept, skipped


def extract_deb(data, out, strip_prefix=""):
    strip = _normal(strip_prefix)
    written = 0
    with tarfile.open(data, mode="r|xz") as tar:
        for member in tar:
            name = _normal(member.name)
            if strip:
                if not name.startswith(strip + "/"):
                    continue
                name = name[len(strip) + 1:]
            if not name:
                continue
            member.name = name
            if member.issym() and member.linkname.startswith("/"):
                if strip:
                    raise ExtractError(
                        f"{data}: {name} is an absolute link, which cannot"
                        " be relative once the directory is flattened")
                member.linkname = os.path.relpath(
                    member.linkname.lstrip("/"), os.path.dirname(name) or ".")
            elif member.islnk():
                member.linkname = _normal(member.linkname)
                if strip:
                    member.linkname = member.linkname[len(strip) + 1:]
            written += 1
            tar.extract(member, out, filter="data")
    if not written:
        raise ExtractError(f"{data}: nothing was unpacked (wrong prefix?)")
    for root, dirs, files in os.walk(out):
        for entry in dirs + files:
            path = os.path.join(root, entry)
            if os.path.islink(path) and os.readlink(path).startswith("/"):
                raise ExtractError(f"{path}: an absolute link is left")
    return written


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = parser.add_subparsers(dest="mode", required=True)
    llvm = sub.add_parser("llvm")
    llvm.add_argument("archive")
    llvm.add_argument("strip_prefix")
    llvm.add_argument("out")
    deb = sub.add_parser("deb")
    deb.add_argument("archive")
    deb.add_argument("out")
    deb.add_argument("--strip-prefix", default="")
    args = parser.parse_args(argv)
    try:
        if args.mode == "llvm":
            kept, skipped = extract_llvm(args.archive, args.strip_prefix,
                                         args.out)
            print(f"extract.py: {kept} members written, {skipped >> 20} MiB"
                  " skipped")
        else:
            extract_deb(args.archive, args.out, args.strip_prefix)
    except ExtractError as e:
        print(f"extract.py: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
