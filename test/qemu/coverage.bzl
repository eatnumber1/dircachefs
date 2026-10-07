"""Coverage plumbing of the QEMU test macros (step 7.2).

Under `bazel coverage` (--config=coverage) the test binaries and dcfs are
built with clang's source-based coverage; the guest writes their .profraw
files, ships them over the serial console, and run-qemu.sh turns them into an
lcov file in COVERAGE_DIR (scripts/cov-lcov.sh). In any other build these
return nothing.
"""

COVERAGE_BUILD = "//test/qemu:coverage_build"

# The binaries of the shared e2e initramfs that are our code.
E2E_COVERAGE_OBJECTS = [
    "//dcfs:main_static",
    "//tools:fhtest",
    "//tools:testutil",
    "//bench:dcfs_bench",
]

_TOOLS = [
    "//test/qemu:scripts/cov-lcov.sh",
    "@llvm_toolchain_llvm//:bin/llvm-cov",
    "@llvm_toolchain_llvm//:bin/llvm-profdata",
]

def coverage_data(objects):
    """Runfiles for run-qemu.sh's coverage step: the LLVM tools and the instrumented binaries."""
    return select({
        COVERAGE_BUILD: _TOOLS + objects,
        "//conditions:default": [],
    })

def coverage_args(objects):
    """run-qemu.sh flags naming the tools and the instrumented binaries."""
    return select({
        COVERAGE_BUILD: [
            "--cov-script",
            "$(rootpath //test/qemu:scripts/cov-lcov.sh)",
            "--cov-llvm-profdata",
            "$(rootpath @llvm_toolchain_llvm//:bin/llvm-profdata)",
            "--cov-llvm-cov",
            "$(rootpath @llvm_toolchain_llvm//:bin/llvm-cov)",
        ] + [
            a
            for o in objects
            for a in ["--cov-object", "$(rootpath %s)" % o]
        ],
        "//conditions:default": [],
    })
