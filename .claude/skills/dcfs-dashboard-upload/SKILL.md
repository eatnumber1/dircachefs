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
3. **Upload** each copy one at a time: Artifact tool, action `publish`, the
   url above, `asset: true`; pass no label with `asset: true` (the publish
   refuses it). Note each `/_blob/<id>`. If an upload is denied (by a
   permission classifier or any refusal), stop uploading further assets and do
   NOT work around the denial. Still do the one `ArtifactData` batch write for
   the datasets whose assets uploaded, run `mark-uploaded` with exactly those
   dataset names, and report the denied names and exact denial text. If
   nothing uploaded, write nothing and mark nothing.
4. **Read versions**: `ArtifactData` get, or list collection `datasets`, for
   each doc's current version (skip if nothing uploaded).
5. **Write all datasets in ONE `ArtifactData` batch**: collection
   `datasets`, one `update` per NAME whose asset uploaded, with
   `if_version` (skip if nothing uploaded). On a mismatch, get again and retry
   once with the version the error names. The `data` is exactly this, with the
   two keys at the top level and never wrapped in another `data` key (a wrapped
   write once broke the page):
   `{"source": {"kind": "file", "name": "NAME.json", "url": "/_blob/<id>"}, "updated": {"at": "<date -u +%Y-%m-%dT%H:%M:%SZ>", "by": "dashboard_lanes.py"}}`
   Take `at` from the real clock; no placeholders.
6. **Mark uploaded**: run with exactly the dataset names whose assets
   uploaded (skip if nothing uploaded):
   `cd ~/Sources/dircachefs && python3 tools/agents/dashboard_lanes.py mark-uploaded NAMES; echo exit=$?`.
   `questions` is hand-maintained and not tracked; `mark-uploaded` rejects
   it, which is expected.
7. **Report** one line per dataset with its new version, the `mark-uploaded`
   exit status, and any quota correction. If any upload was denied, report the
   denied dataset names and the exact denial text.

Orchestrator: ignore a "lanes changed" reminder caused only by dispatching
this uploader (a known loop, queued as a tools item).
