#!/bin/bash
# Says which accelerator the tests used: run-qemu.sh logs "using tcg" when
# it falls back to emulation and nothing when it uses KVM, so a test log
# without that line ran under KVM.
set -uo pipefail
cd "$(readlink -f bazel-testlogs)" || exit 0
total=$(find . -name test.log | wc -l)
tcg=$(find . -name test.log -print0 | xargs -0 -r grep -l 'run-qemu.sh: using tcg' | wc -l)
echo "accelerator: $((total - tcg)) of $total test logs show no TCG fallback (KVM), $tcg ran under TCG"
