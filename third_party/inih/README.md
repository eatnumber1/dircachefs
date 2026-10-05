# third_party/inih: private static libinih for xfsprogs

inih r62 (two source files, built as a `cc_library`), used only to link
`third_party/xfsprogs` (`mkfs.xfs -c` config-file parsing). Pin, checksum and
caveat about GitHub's generated archives: `third_party/xfsprogs/README.md`
("Dependencies").

Updating the pin: new tag and sha256 in `MODULE.bazel`'s `inih_src`, then
`bazel test //third_party/xfsprogs/...`.
