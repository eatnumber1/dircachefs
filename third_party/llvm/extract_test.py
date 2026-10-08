"""Tests for extract.py: what the skip list keeps (plan step 7.1b).

Run as extract_test.py <BUILD.llvm_repo.tpl of toolchains_llvm>. The skip list
was wrong once (clangd, which toolchains_llvm symlinks, was skipped; the
discovery cost a 26-minute refetch), so these build a small release-shaped
archive and check that nothing toolchains_llvm 1.11.1 refers to is dropped,
that what is dropped is not written, and that no written link dangles.
"""

import io
import os
import re
import sys
import tarfile
import tempfile
import unittest

import extract

TEMPLATE = None  # the text of BUILD.llvm_repo.tpl, filled in main()

PREFIX = "LLVM-22.1.8-Linux-X64"

# toolchains_llvm 1.11.1, toolchain/internal/configure.bzl `_toolchain_tools`
# (symlinked into the generated repository's bin/), and the tools the repo
# BUILD file's fixed srcs and our own targets use (the template is read for
# the rest).
KNOWN_NEEDED = [
    "bin/clang", "bin/clang-22", "bin/clang++", "bin/clang-cpp",
    "bin/clangd", "bin/clang-format", "bin/clang-tidy", "bin/clang-query",
    "bin/clang-apply-replacements", "bin/clang-include-cleaner",
    "bin/clang-scan-deps", "bin/ld.lld", "bin/ld64.lld", "bin/lld",
    "bin/llvm-ar", "bin/llvm-as", "bin/llvm-cov", "bin/llvm-dwp",
    "bin/llvm-nm", "bin/llvm-objcopy", "bin/llvm-objdump",
    "bin/llvm-profdata", "bin/llvm-ranlib", "bin/llvm-readelf",
    "bin/llvm-readobj", "bin/llvm-strip", "bin/llvm-symbolizer",
    "lib/x86_64-unknown-linux-gnu/libc++.a",
    "lib/x86_64-unknown-linux-gnu/libc++abi.a",
    "lib/x86_64-unknown-linux-gnu/libunwind.a",
    "lib/clang/22/lib/x86_64-unknown-linux-gnu/libclang_rt.builtins.a",
    "lib/clang/22/lib/x86_64-unknown-linux-gnu/libclang_rt.asan.a",
    "lib/clang/22/include/stddef.h", "include/c++/v1/vector",
    "include/x86_64-unknown-linux-gnu/c++/v1/__config_site",
    "lib/libclang-cpp.so.22.1", "share/clang/clang-format-diff.py",
]

# Never needed: all in the release, all skipped.
KNOWN_SKIPPED = [
    "bin/flang-22", "bin/mlir-opt", "bin/lldb", "bin/opt", "bin/llc",
    "bin/llvm-ml", "bin/llvm-bolt", "bin/clang-repl",
    "lib/libLLVMSupport.a", "lib/libclangAST.a", "lib/libMLIRIR.a",
    "lib/liblldb.so.22.1.8", "lib/libclang.so.22.1.8",
    "lib/libLTO.so.22.1", "lib/cmake/llvm/LLVMConfig.cmake",
    "include/llvm/ADT/StringRef.h", "include/clang/Basic/LLVM.h",
]

# (link, target): kept links, and one whose target is skipped.
LINKS = [
    ("bin/clang", "clang-22"), ("bin/clang++", "clang"),
    ("bin/ld.lld", "lld"), ("bin/llvm-readelf", "llvm-readobj"),
    ("lib/libclang-cpp.so", "libclang-cpp.so.22.1"),
]
DANGLING_LINKS = [("bin/llvm-ml64", "llvm-ml"),
                  ("lib/libclang.so", "libclang.so.22.1.8")]


def template_paths():
    """The explicit bin/, lib/ and share/ paths BUILD.llvm_repo.tpl names that
    a Linux build uses (not the Darwin ones, nor the optional libclang)."""
    paths = set()
    for path in re.findall(r'"((?:bin|lib|share)/[^"*]*)"', TEMPLATE):
        path = path.replace("{LLVM_VERSION}", "22")
        if "darwin" in path or path.endswith(".dylib"):
            continue
        if path.startswith("lib/libclang.so"):
            continue
        paths.add(path)
    return paths


def build_release(path, names, links):
    with tarfile.open(path, "w:xz") as tar:
        tar.addfile(tarfile.TarInfo(PREFIX + "/"))
        for name in names:
            info = tarfile.TarInfo(f"{PREFIX}/{name}")
            data = b"x"
            info.size = len(data)
            info.mode = 0o755
            tar.addfile(info, io.BytesIO(data))
        for name, target in links:
            info = tarfile.TarInfo(f"{PREFIX}/{name}")
            info.type = tarfile.SYMTYPE
            info.linkname = target
            tar.addfile(info)


class ExtractLlvmTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.archive = os.path.join(self.tmp.name, "llvm.tar.xz")
        self.out = os.path.join(self.tmp.name, "out")
        os.mkdir(self.out)
        files = set(KNOWN_NEEDED) | set(KNOWN_SKIPPED) | template_paths()
        # The template also names directories (include/c++, lib/clang/22/lib):
        # the files below them make them.
        files = {f for f in files
                 if not any(g.startswith(f + "/") for g in files)}
        linked = {name for name, _ in LINKS + DANGLING_LINKS}
        # Plain files only where the archive has no link of the same name.
        build_release(self.archive, sorted(files - linked),
                      LINKS + DANGLING_LINKS)

    def run_extract(self):
        return extract.extract_llvm(self.archive, PREFIX, self.out)

    def test_nothing_toolchains_llvm_needs_is_skipped(self):
        self.run_extract()
        for name in sorted((set(KNOWN_NEEDED) | template_paths())
                           - {n for n, _ in DANGLING_LINKS}):
            self.assertTrue(os.path.lexists(os.path.join(self.out, name)),
                            f"{name} is skipped")

    def test_the_template_names_paths(self):
        # The extraction of the template is not vacuous.
        self.assertIn("bin/clang", template_paths())
        self.assertIn("bin/llvm-readelf", template_paths())
        self.assertGreater(len(template_paths()), 15)

    def test_what_is_skipped_is_not_written(self):
        self.run_extract()
        for name in KNOWN_SKIPPED:
            self.assertFalse(os.path.lexists(os.path.join(self.out, name)),
                             f"{name} is written")

    def test_no_written_link_dangles(self):
        self.run_extract()
        links = []
        for root, dirs, files in os.walk(self.out):
            for entry in dirs + files:
                path = os.path.join(root, entry)
                if os.path.islink(path):
                    links.append(path)
        self.assertGreaterEqual(len(links), len(LINKS))
        for path in links:
            self.assertTrue(os.path.exists(path), f"{path} dangles")
        for name, _ in DANGLING_LINKS:
            self.assertFalse(os.path.lexists(os.path.join(self.out, name)))

    def test_a_chain_of_links_to_a_skipped_file_fails(self):
        # bin/ml -> llvm-ml64 -> llvm-ml (skipped): llvm-ml64 is not skipped
        # itself, so only the walk after the extraction sees the chain dangle.
        chain = os.path.join(self.tmp.name, "chain.tar.xz")
        build_release(chain, ["bin/clang-22", "bin/llvm-ml"],
                      [("bin/llvm-ml64", "llvm-ml"),
                       ("bin/ml", "llvm-ml64")])
        with self.assertRaisesRegex(extract.ExtractError, "bin/ml"):
            extract.extract_llvm(chain, PREFIX, self.out)

    def test_a_hard_link_outside_the_prefix_fails(self):
        hard = os.path.join(self.tmp.name, "hard.tar.xz")
        with tarfile.open(hard, "w:xz") as tar:
            info = tarfile.TarInfo(f"{PREFIX}/bin/clang-22")
            info.size = 1
            tar.addfile(info, io.BytesIO(b"x"))
            info = tarfile.TarInfo(f"{PREFIX}/bin/clang")
            info.type = tarfile.LNKTYPE
            info.linkname = "OTHER/bin/clang-22"
            tar.addfile(info)
        with self.assertRaisesRegex(extract.ExtractError, "hard link"):
            extract.extract_llvm(hard, PREFIX, self.out)

    def test_a_wrong_strip_prefix_fails(self):
        with self.assertRaisesRegex(extract.ExtractError, "no member"):
            extract.extract_llvm(self.archive, "LLVM-0-Linux-X64", self.out)

    def test_main_reports_an_error_and_exits_nonzero(self):
        self.assertEqual(
            1, extract.main(["llvm", self.archive, "nope", self.out]))
        self.assertEqual(
            0, extract.main(["llvm", self.archive, PREFIX, self.out]))


def build_deb_data(path, members):
    """members: [(name, type, content or link target)]."""
    with tarfile.open(path, "w:xz") as tar:
        for name, kind, content in members:
            info = tarfile.TarInfo(name)
            if kind == "sym":
                info.type = tarfile.SYMTYPE
                info.linkname = content
                tar.addfile(info)
            elif kind == "dir":
                info.type = tarfile.DIRTYPE
                info.mode = 0o755
                tar.addfile(info)
            else:
                info.size = len(content)
                tar.addfile(info, io.BytesIO(content))


class ExtractDebTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.data = os.path.join(self.tmp.name, "data.tar.xz")
        self.out = os.path.join(self.tmp.name, "out")
        os.mkdir(self.out)

    def test_absolute_links_become_relative(self):
        build_deb_data(self.data, [
            ("./lib/x86_64-linux-gnu/ld-linux-x86-64.so.2", "file", b"ld"),
            ("./lib64/ld-linux-x86-64.so.2", "sym",
             "/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2"),
            ("./usr/lib/x86_64-linux-gnu/libm.so.6", "sym", "libm-2.36.so"),
            ("./usr/lib/x86_64-linux-gnu/libm-2.36.so", "file", b"m"),
        ])
        extract.extract_deb(self.data, self.out)
        link = os.path.join(self.out, "lib64/ld-linux-x86-64.so.2")
        self.assertEqual("../lib/x86_64-linux-gnu/ld-linux-x86-64.so.2",
                         os.readlink(link))
        self.assertEqual(b"ld", open(link, "rb").read())
        self.assertEqual("libm-2.36.so", os.readlink(os.path.join(
            self.out, "usr/lib/x86_64-linux-gnu/libm.so.6")))

    def test_a_strip_prefix_flattens_and_skips_the_rest(self):
        build_deb_data(self.data, [
            ("./usr/lib/x86_64-linux-gnu/libz.so.1.2.13", "file", b"z"),
            ("./usr/lib/x86_64-linux-gnu/libz.so.1", "sym", "libz.so.1.2.13"),
            ("./usr/share/doc/zlib1g/copyright", "file", b"c"),
        ])
        extract.extract_deb(self.data, self.out, "usr/lib/x86_64-linux-gnu")
        self.assertEqual(["libz.so.1", "libz.so.1.2.13"],
                         sorted(os.listdir(self.out)))

    def test_an_absolute_link_cannot_be_flattened(self):
        build_deb_data(self.data, [
            ("./usr/lib/x86_64-linux-gnu/libx.so", "sym", "/lib/libx.so.1"),
        ])
        with self.assertRaisesRegex(extract.ExtractError, "absolute link"):
            extract.extract_deb(self.data, self.out, "usr/lib/x86_64-linux-gnu")

    def test_nothing_unpacked_fails(self):
        build_deb_data(self.data, [("./usr/share/x", "file", b"x")])
        with self.assertRaisesRegex(extract.ExtractError, "nothing"):
            extract.extract_deb(self.data, self.out, "usr/lib")

    def test_usrmerge_links_lib_and_lib64_into_usr(self):
        # Debian 13 ships merged-usr packages: libc.so's linker script names
        # /lib/x86_64-linux-gnu/libc.so.6 and /lib64/ld-linux-x86-64.so.2,
        # which a sysroot of the packages alone does not have (step 7.1c).
        build_deb_data(self.data, [
            ("./usr/lib/x86_64-linux-gnu/libc.so.6", "file", b"c"),
            ("./usr/lib64/ld-linux-x86-64.so.2", "sym",
             "../lib/x86_64-linux-gnu/ld-linux-x86-64.so.2"),
            ("./usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2", "file", b"ld"),
        ])
        extract.extract_deb(self.data, self.out)
        extract.usrmerge(self.out)
        self.assertEqual("usr/lib", os.readlink(os.path.join(self.out, "lib")))
        self.assertEqual(b"c", open(os.path.join(
            self.out, "lib/x86_64-linux-gnu/libc.so.6"), "rb").read())
        self.assertEqual(b"ld", open(os.path.join(
            self.out, "lib64/ld-linux-x86-64.so.2"), "rb").read())

    def test_usrmerge_keeps_a_real_lib_directory(self):
        build_deb_data(self.data, [
            ("./lib/x86_64-linux-gnu/libc.so.6", "file", b"c"),
            ("./usr/lib/x86_64-linux-gnu/libc.a", "file", b"a"),
        ])
        extract.extract_deb(self.data, self.out)
        extract.usrmerge(self.out)
        self.assertFalse(os.path.islink(os.path.join(self.out, "lib")))
        self.assertFalse(os.path.lexists(os.path.join(self.out, "lib64")))


if __name__ == "__main__":
    with open(sys.argv.pop(1), encoding="utf-8") as f:
        TEMPLATE = f.read()
    unittest.main()
