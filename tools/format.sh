#!/bin/sh
# Formats all tracked C/C++ and Bazel source files in the repository.
set -eu

if command -v clang-format >/dev/null 2>&1; then
  git ls-files -- '*.cc' '*.h' '*.c' | xargs -r clang-format -i
else
  echo "clang-format not found; skipping C/C++ formatting."
fi

if command -v buildifier >/dev/null 2>&1; then
  git ls-files -- '*.bazel' '*.bzl' 'MODULE.bazel' 'WORKSPACE' | xargs -r buildifier
else
  echo "buildifier not found; skipping Bazel formatting."
fi
