#!/bin/bash
# Prepares the `osv` job (plan step 5.3): checks osv-scanner.toml's ignores
# (every one needs a reason and an expiry date that has not passed) and
# writes the SBOM of every pin (tools/sbom/README.md) to osv/dcfs.cdx.json,
# which the scanner action then scans. Uses the runner's python3 (3.11 or
# newer: tomllib); //tools/sbom:sbom_test runs the same code under Bazel's
# hermetic interpreter.
set -euo pipefail
cd "$(dirname "$0")/../.."
python3 tools/sbom/sbom.py check-ignores --config osv-scanner.toml
mkdir -p osv
python3 tools/sbom/sbom.py generate --out osv/dcfs.cdx.json
