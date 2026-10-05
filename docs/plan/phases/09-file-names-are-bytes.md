# Phase 9 — File names are bytes

**Decision (russ, 2026-10-05).** dcfs handles names as unmodified bytes
end to end. The only bytes a Linux name cannot contain are `\0` and `/`
(some filesystems restrict further, e.g. NAME_MAX = 255 bytes). WTF-8 is
not an option here: it round-trips ill-formed UTF-16, not arbitrary bytes
(a lone 0xFF byte has no WTF-8 form), so raw bytes it is.
- Already true in storage: dentry names, xattr names, symlink targets and
  boundary names are `BLOB` columns. Audit every path a name travels
  (FUSE request to SQLite to backing syscall and back; readdir;
  xattr list; symlink targets; handles) for anything that assumes text:
  `std::string` is fine, C-string functions (`strlen`, `%s`) are not.
- Where names are printed, escape them, never raw: logs (a name containing
  a newline must not forge a log line; use a fixed escaping, e.g.
  `absl::CHexEscape`), mountinfo parsing (`\040`-style octal escapes),
  `dcfs exports` output and suggested fstab lines (octal escapes as
  exports(5)/fstab(5) expect).
- Tests first, built from where a name can break rather than from every
  byte value. A name passes through: SQLite keys (memcmp ordering, BLOB
  binding), C++ string handling (anything that stops at a byte or decodes
  text), the kernel's path walk (`/`, `.`, `..`), our own text formats
  (log lines, mountinfo, fstab and exports lines, all whitespace- or
  newline-delimited with backslash escapes), and length limits. One
  representative per hazard:
  - **Delimiters of our formats:** `\n`, `\r`, `\t`, a single space,
    leading and trailing spaces, `#`, `,`, `=`, `:`, `\`, `"`; names
    that look like escapes (`\040`, `\n` as two characters, `%2F`).
  - **Byte classes at their edges:** 0x01 and 0x1F (controls), 0x7F,
    0x80 and 0xFF (high bit), each alone and inside a longer name.
  - **Text-decoding hazards:** invalid UTF-8 (lone continuation byte, a
    lead byte truncated at the end of the name, the overlong encoding of
    `/` (0xC0 0xAF) which must stay an ordinary byte pair, an encoded
    surrogate (0xED 0xA0 0x80)); valid multi-byte UTF-8; NFC and NFD forms
    of the same word in one directory (two different files: no
    normalization); names differing only in case.
  - **Path-walk specials:** `...`, `.x`, `x.`, `. ` and ` .` (but `.`
    and `..` themselves are never names: creating them fails as on the
    backing filesystem).
  - **Ordering and prefixes:** `a`, `a\x80`, `a\xff`, `aa` in one
    directory (memcmp order differs from any text collation); readdir of a
    complete cached directory lists exactly the backing names, byte for
    byte.
  - **Lengths:** 1 byte; exactly 255 bytes (NAME_MAX); 256 (ENAMETOOLONG,
    matching the backing filesystem); 255 bytes ending in the middle of a
    multi-byte character; a directory chain deeper than PATH_MAX in total
    (dcfs works by fds, so lookups and handles still work there).
  - **Symlink targets and xattrs:** targets containing every hazard above
    plus `/` sequences (`//`, `/./`, trailing `/`), up to 4095 bytes;
    xattr names (`user.` plus hazards) and binary values containing NUL
    bytes.
  - **Borrowed from xfstests generic/453 and generic/454** (names and
    xattr names that render alike but are different bytes; written as our
    own test, since xfstests is GPL-2.0, with credit in a comment): NFC,
    NFD, NFKC and NFKD forms of one character; full-width vs. half-width
    forms; a ligature against its long expansion; the overlong "fake
    slash"; emoji (4-byte UTF-8); a multi-line name drawn with box
    characters; right-to-left override and zero-width characters. Each
    set must give distinct directory entries and distinct xattrs.
  - **Already covered elsewhere, not duplicated:** pjdfstest (in the suite
    today) tests NAME_MAX and PATH_MAX limits per operation; the xfstests
    subset will run generic/453 and generic/454 themselves. The corpus
    keeps only the length cases with a dcfs-specific angle (warm-cache
    readdir and NFS handles of 255-byte names, chains deeper than
    PATH_MAX through handles).
  Each corpus name goes through one round-trip suite rather than a
  hand-written test per operation: create file and directory, lookup,
  readdir (exact bytes), stat, rename to another corpus name, link,
  symlink, xattr set/get/list, open/read/write, unlink/rmdir, NFS handle,
  and a dcfs restart with a warm cache (served with zero backing reads).
  Run on ext4, xfs and btrfs. Plus a seeded random test: names of random
  bytes (any byte but `\0` and `/`, lengths 1-255), created through dcfs
  and directly on the backing filesystem, compared both ways.
  **Budget** (limited compute, no months-long runs): one guest per
  filesystem runs the whole corpus suite; renames chain each name into
  the next (n renames, not n^2 pairs); the corpus is about 60 names; the
  random test uses 1,000 names in the medium tier and 100,000 only in the
  slow tier. Target: under a minute per filesystem with KVM.
  - **Printing:** a newline name in a log line shows the escape and does
    not start a new line; mount points and sources containing space, tab,
    newline, `#` and `\` round-trip through mountinfo parsing, `dcfs
    exports` and the suggested fstab lines (octal escapes); unit tests for
    the escaping functions.
Owner: Sonnet, Opus review. Order: right after Phase 8 (Coverage close to 100%),
so later phases build on it.
