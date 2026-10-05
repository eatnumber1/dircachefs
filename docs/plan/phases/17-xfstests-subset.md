# Phase 17 — xfstests subset

After Phase 15 (The `mount.dcfs` wrapper), which gives xfstests a mount helper:
`FSTYP=fuse`, `FUSE_SUBTYP=.dcfs`): run the generic tests
that apply, with per-filesystem expected-failure lists in the style of
pjdfstest's; triage every failure as a dcfs bug (test first, then fix) or
a documented limitation.
Owner: Sonnet. Order: after Phase 15 (The `mount.dcfs` wrapper), which gives xfstests a mount helper.
