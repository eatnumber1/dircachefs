"""Unpacks LLVM's release tarball without what no dcfs build step uses.

    extract.py <archive.tar.xz> <strip prefix> <output directory>

The Linux x86_64 release is 1.9 GB packed and 12 GB unpacked, most of it the
static libraries of LLVM, clang, MLIR and flang, lldb and the tools of
flang, MLIR and the clang tooling libraries: unpacking all of it took 12 to
17 minutes of every pin change. The members matched by SKIP are not written.
Everything the toolchain uses is kept: clang, lld, the llvm-* binutils,
llvm-profdata and llvm-cov, clang-tidy, clang-format and clang-query, the
headers, libc++, libunwind and compiler-rt.
"""

import re
import sys
import tarfile

SKIP = re.compile(
    "|".join(
        [
            # Static libraries of LLVM, clang, MLIR, flang, lld, Polly (the
            # runtime libraries are in lib/<triple>/ and lib/clang/).
            r"lib/[^/]+\.a",
            r"lib/(cmake|python[0-9.]*|objects-RELEASE|libear|libscanbuild)(/.*)?",
            r"lib/(liblldb[^/]*|libclang\.so[^/]*|libLTO\.so[^/]*|LLVMPolly\.so"
            r"|libRemarks\.so[^/]*|libmlir_[^/]*)",
            # Headers of the compiler libraries (libc++'s are include/c++).
            r"include/(llvm|llvm-c|clang|clang-c|mlir|mlir-c|flang|lld|lldb"
            r"|polly|clang-tidy|clang-include-fixer)(/.*)?",
            # Tools nothing here runs.
            r"bin/(mlir-[^/]*|flang[^/]*|f18[^/]*|fir-[^/]*|bbc|tco|lldb[^/]*"
            r"|clang-repl|clang-check|clangd|clang-doc|clang-change-namespace"
            r"|clang-move|clang-include-fixer|clang-reorder-fields"
            r"|clang-refactor|clang-extdef-mapping|clang-installapi"
            r"|clang-nvlink-wrapper|clang-sycl-linker|clang-linker-wrapper"
            r"|find-all-symbols|c-index-test|modularize|pp-trace|bugpoint"
            r"|opt|llc|lli|llvm-lto[0-9]*|llvm-reduce|llvm-ir2vec"
            r"|llvm-dwarfutil|llvm-c-test|llvm-exegesis|llvm-bolt[^/]*"
            r"|llvm-jitlink|llvm-split|llvm-gsymutil|llvm-cfi-verify"
            r"|llvm-profgen|llvm-mca|llvm-rtdyld|llvm-debuginfo-analyzer"
            r"|llvm-ml|sancov|dsymutil|perf2bolt|merge-fdata)",
        ]
    )
)


def main(argv):
    archive, prefix, out = argv[1:4]
    prefix = prefix.rstrip("/") + "/"
    skipped = kept = 0
    with tarfile.open(archive, mode="r|xz") as tar:
        for member in tar:
            if not member.name.startswith(prefix):
                continue
            member.name = member.name[len(prefix):]
            if not member.name:
                continue
            if SKIP.fullmatch(member.name):
                skipped += member.size
                continue
            if member.islnk():
                member.linkname = member.linkname[len(prefix):] if (
                    member.linkname.startswith(prefix)) else member.linkname
            kept += member.size
            tar.extract(member, out, filter="fully_trusted")
    print(f"extract.py: kept {kept >> 20} MiB, skipped {skipped >> 20} MiB")


if __name__ == "__main__":
    main(sys.argv)
