---
name: dcfs-dashboard-upload
description: Upload dcfs Mission Control dashboard datasets. Use when the Stop or PostToolUse hook says "Dashboard <names> changed: upload ...".
---

# Upload dashboard datasets

Dashboard: https://claude.ai/artifact/EVypNNcogbNcxzRjBYp8rK. Sources:
`~/.cache/dcfs-dashboard/NAME.json`. NAMES are the datasets the hook lists.
The rule is in the Merge paragraph of
[process](/docs/plan/process.md). Do the whole job yourself; do not edit the
repository.

1. **Quota first** (only if `quota` is listed). Run
   `cd ~/Sources/dircachefs && timeout 90 python3 -I tools/agents/claude_usage.py 2>&1 | grep -a -A1 -E "Currentweek\(allmodels\)|Currentsession"`.
   If quota.json's "Claude week" or "Claude session" `used_pct` differs from
   the live value, fix it with the Edit tool (the known parser bug records 0).
   If budget.json's newest actual is 0 from that bug, fix that too. Check
   each with `python3 -m json.tool`.
2. **Copy** each NAME.json with `cp -f` into the session scratchpad (or
   `mktemp -d`). Never write into the repository.
3. **Upload** each copy: Artifact tool, action `publish`, the url above,
   `asset: true`. Note each `/_blob/<id>`.
4. **Read versions**: `ArtifactData` get, or list collection `datasets`, for
   each doc's current version.
5. **Write all datasets in ONE `ArtifactData` batch**: collection
   `datasets`, one `update` per NAME with `if_version`. On a mismatch, get
   again and retry once with the version the error names. The `data` is
   exactly this, with the two keys at the top level and never wrapped in
   another `data` key (a wrapped write once broke the page):
   `{"source": {"kind": "file", "name": "NAME.json", "url": "/_blob/<id>"}, "updated": {"at": "<date -u +%Y-%m-%dT%H:%M:%SZ>", "by": "dashboard_lanes.py"}}`
   Take `at` from the real clock; no placeholders.
6. **Mark uploaded**:
   `cd ~/Sources/dircachefs && python3 tools/agents/dashboard_lanes.py mark-uploaded NAMES; echo exit=$?`.
   `questions` is hand-maintained and not tracked; `mark-uploaded` rejects
   it, which is expected.
7. **Report** one line per dataset with its new version, the `mark-uploaded`
   exit status, and any quota correction.

Orchestrator: ignore a "lanes changed" reminder caused only by dispatching
this uploader (a known loop, queued as a tools item).
