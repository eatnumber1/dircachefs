#!/bin/bash
# Step 7.1b: nothing a C/C++ action takes from outside the Bazel workspace
# comes from the host but libc (third_party/llvm/README.md). $1 is the probe
# tools/cc_toolchain_probe.txt, which the genrule of that name makes with the
# toolchain's $(CC): the include search list, where the C library files are
# found, and the shared libraries the compiler and linker load.
set -euo pipefail

probe=$1
failed=0

fail() {
  echo "FAIL: $1" >&2
  failed=1
}

# The probe's sections are headed `== name`.
section() {
  sed -n "/^== $1\$/,/^== /p" "${probe}" | sed '1d;$d'
}

# Host directories: a path under one of them is the host's.
is_host_path() {
  [[ $1 == /usr/* || $1 == /lib/* || $1 == /lib64/* || $1 == /opt/* ||
    $1 == /etc/* ]]
}

include_dirs=$(section include | sed -n 's/^ //p')
[[ -n ${include_dirs} ]] || fail "no include search list in the probe"
while IFS= read -r dir; do
  if [[ -n ${dir} ]] && is_host_path "${dir}"; then
    fail "the include search list has the host's ${dir}"
  fi
done <<<"${include_dirs}"

# -print-file-name prints the bare name when the file is not found.
while IFS== read -r name path; do
  [[ -n ${name} ]] || continue
  if [[ ${path} != */sysroot/* ]]; then
    fail "${name} is not found in the sysroot: ${path}"
  fi
done < <(section files)

# `calling init: <path>` is ld.so's LD_DEBUG=libs line for each library it
# initializes, for the wrapper's bash, clang and lld alike. The host provides
# glibc (its loader, libraries and NSS modules, which coreutils load) for the
# compiler and linker, and bash's own libtinfo.
allowed='/(ld-linux-x86-64|lib(c|m|dl|pthread|rt|resolv|util|nss_[a-z]+|tinfo))\.so\.[0-9]+$'
while IFS= read -r line; do
  path=${line#*calling init: }
  [[ ${path} == /* ]] || continue
  if is_host_path "${path}" && ! [[ ${path} =~ ${allowed} ]]; then
    fail "the compiler or linker loads the host's ${path}"
  fi
done < <(section loaded | grep 'calling init:' || true)

if ! section link | grep -q '^LINK-OK$'; then
  fail "the probe program did not link: $(section link | tail -3)"
fi
exit "${failed}"
