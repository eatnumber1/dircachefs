"""What scripts/mkinitramfs.sh needs to install a dynamic binary's libraries.

A guest binary linked dynamically (the ASan and UBSan builds) runs the glibc
of the pinned toolchain's sysroot, not the host's: mkinitramfs.sh reads the
binary's ELF headers with the toolchain's llvm-readelf and copies the
interpreter and the libraries from the sysroot (step 7.1b).
"""

# Environment of the mkinitramfs.sh command line (in a genrule's `cmd`).
GUEST_LIBS_ENV = (
    "DCFS_SYSROOT=$(location @dcfs_llvm//sysroot) " +
    "DCFS_READELF=$(location @dcfs_llvm//:bin/llvm-readelf)"
)

# Add to the genrule's `tools`.
GUEST_LIBS_TOOLS = [
    "@dcfs_llvm//:bin/llvm-readelf",
    "@dcfs_llvm//sysroot",
]
