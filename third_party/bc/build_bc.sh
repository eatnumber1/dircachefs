#!/bin/sh
# Step 3.1a: build GNU bc as an ordinary Bazel action (see BUILD.bazel's
# :bc_build genrule and README.md for why bc is fetched and built this way
# instead of coming from the host or the BCR).
set -eu

# Bazel hands us paths relative to the execroot; resolve them to absolute
# paths before we cd into $BUILD below, or they break.
SRC_ROOT=$(cd "$1" && pwd)
OUT_BC=$(readlink -f "$(dirname "$2")")/$(basename "$2")

BUILD=$(mktemp -d)

cd "$BUILD"
# bc's release tarball ships pre-generated bc/bc.c and bc/scan.c (from
# bc.y/scan.l), newer than their sources, so this build never actually
# invokes flex/bison (configure's AC_PROG_LEX/AC_PROG_YACC checks just
# probe for their presence; see README.md's "Hermetic build tools" for why
# they stay host tools here, same as the kernel build).
"$SRC_ROOT/configure" --without-libedit --without-readline
make -C lib
make -C bc

cp "$BUILD/bc/bc" "$OUT_BC"
