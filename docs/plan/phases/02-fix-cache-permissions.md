# Phase 2 — Fix: world-readable cache database

`main.cc` creates the cache database with mode 0644 after setting the
daemon's umask to 0, and SQLite gives its `-wal` and `-shm` files the main
file's mode. Any local user can therefore read every cached name,
attribute, xattr and symlink target, including those of directories they
cannot list.
- Test first (QEMU): after dcfs starts, the database, `-wal` and `-shm`
  files are mode 0600 and owned by root; an unprivileged user (`testutil
  runas`) cannot open them. Fails today.
- Fix: create the database 0600; dcfs creates the directory holding
  the database 0700 if it does not exist (and warns if an existing one is group- or
  world-accessible). README notes the cache holds metadata as sensitive as
  the backing tree's.
Owner: Sonnet. Order: first, before everything else (small, security).
