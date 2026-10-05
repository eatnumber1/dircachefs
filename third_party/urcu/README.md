# third_party/urcu: private static liburcu for xfsprogs

userspace-rcu 0.15.7, `configure --disable-shared --enable-static`, used only
to link `third_party/xfsprogs`. Pin, checksum, flags and the C++ probe quirk:
`third_party/xfsprogs/README.md` ("Dependencies").

Updating the pin: new version and sha256 (compare with the `.sha256` file next
to the tarball in <https://lttng.org/files/urcu/>) in `MODULE.bazel`'s
`userspace_rcu_src`, then `bazel test //third_party/xfsprogs/...` (plain and
`--config=asan`).
