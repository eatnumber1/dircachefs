"""HOST_ONLY_COMPATIBLE: for the tests that need neither root nor the guest.

A test that runs on the host and does not depend on how our code was built
(TLC, the man page and flag checks, the repository-shape and SBOM checks, the
pure shell and Python tests of the tooling) gives the same answer under every
configuration. Bazel nevertheless reruns it whenever the configuration's hash
changes, and a sanitizer or coverage build changes it: the TLC tests alone are
about 20 minutes per rerun. Marking them incompatible with those builds drops
them there (the plain tiers still run them: `bazel test --config=fast //...`
and the rest are unchanged). Step 6.4a.

Not for a test that reads the build's own flags or binaries (the warning-flag
tests, the banned-symbols gate): they say which builds they are for.
"""

HOST_ONLY_COMPATIBLE = select({
    "//test/qemu:asan_build": ["@platforms//:incompatible"],
    "//test/qemu:coverage_build": ["@platforms//:incompatible"],
    "//test/qemu:ubsan_build": ["@platforms//:incompatible"],
    "//conditions:default": [],
})
