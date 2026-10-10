# Trial runbook: does the disk stay down? (2026-10-11)

Step 21.1 by hand, on the dev host (8 cores, 62 GB, Ubuntu 24.04), with a
spare disk that holds nothing precious. Commands are for a root shell
(`sudo -i`) unless they start with a build. `/dev/sdX` is the spare disk;
`/dev/sdX1` its partition. Replace both once you know them (section 3).
Terminal 1 watches, terminal 2 works, terminal 3 follows the log.

Known limits (README "Limitations"; none should bite in the trial):
- **Exclusive access.** Nothing may write to `/srv/raw/trial` while dcfs
  serves it, and do not even list the raw path during the trial: that reads
  the disk and spoils the measurement. Out-of-band changes are unsupported.
- **atime.** A regular file's atime is the backing filesystem's (hence
  `noatime` on the raw mount). Directory and symlink atimes are dcfs's own,
  kept in its database; they can make a sync point (an atime-only dirty row
  syncs after the kernel's dirtytime expiry).
- **Snapshots and mounts below the source** (btrfs subvolumes, snapper's
  `.snapshots`) show as empty stubs answering `ENOTSUP`. Not relevant to an
  ext4 trial.
- **One daemon per cache database** (flock): a second mount of the same
  `trial.db` is refused ("in use by another dcfs process").
- **No `fusectl`** (`/sys/fs/fuse/connections` not mounted): `umount` does not
  wait for the daemon and a mount right after an unmount can fail with
  "in use by another dcfs process". Section 1 checks that it is mounted.
- After a crash the dead mount answers `ENOTCONN`: `umount -l /srv/trial`
  before mounting again.

## 1. Kernel (6.9 or newer)

```
uname -r                                 # today: 6.8.0-146-generic, too old
apt install linux-generic-hwe-24.04      # 6.11 or 6.14 or newer, whatever HWE is now
reboot
```

After the reboot:

```
uname -r                                 # expect 6.9 or higher (6.11 / 6.14 / ...)
ls -l /dev/fuse                          # expect: crw-rw-rw- 1 root root 10, 229 ... /dev/fuse
modinfo fuse | head -3                   # expect a filename: line (or "builtin" in a built-in kernel)
grep CONFIG_FUSE_PASSTHROUGH /boot/config-$(uname -r)    # expect CONFIG_FUSE_PASSTHROUGH=y
```

If `/dev/fuse` is missing, `modprobe fuse` and look again. The mount point
of the fuse control filesystem appears with the first FUSE mount, so check
it again in section 3 after `mount /srv/trial`:

```
findmnt /sys/fs/fuse/connections         # expect one line: fusectl  fusectl
```

If it prints nothing, `mount -t fusectl fusectl /sys/fs/fuse/connections`
(the unmount helper relies on it), and say so in the report.

## 2. Build and install

In the dircachefs checkout (bazelisk pins Bazel 9.2.0):

```
cd ~/Sources/dircachefs && git status --short    # clean checkout of the tree under test
bazel build //dcfs:main_static //man:dcfs.8
```

Expect `Build completed successfully`. One binary is every program; it
dispatches on the name it is run under. The static build is the one to
install:

```
sudo install -m 0755 bazel-bin/dcfs/main_static /usr/local/bin/dcfs
sudo ln -sf /usr/local/bin/dcfs /sbin/mount.dcfs
sudo ln -sf /usr/local/bin/dcfs /sbin/mount.fuse.dcfs
sudo ln -sf /usr/local/bin/dcfs /sbin/umount.fuse.dcfs
sudo ln -sf /usr/local/bin/dcfs /sbin/fsck.dcfs
sudo install -m 0644 bazel-bin/man/dcfs.8 /usr/local/share/man/man8/dcfs.8   # optional
sudo mkdir -p /var/lib/dcfs
```

The README's install section lists the first three links (and an optional
`umount.fuse`, which the trial does not need: `umount /srv/trial` is an
fstab path, so it finds `umount.fuse.dcfs`). The `fsck.dcfs` link is the
name the source dispatches on (`dcfs/main.cc`). There is no install target
or script. Check:

```
dcfs --version                    # expect one line naming the version
mount.dcfs -V                     # same
ls -l /sbin/mount.dcfs /sbin/umount.fuse.dcfs /sbin/fsck.dcfs   # all -> /usr/local/bin/dcfs
```

Install `hdparm`, `smartmontools` and (for the cache query) `sqlite3` if
missing: `apt install hdparm smartmontools sqlite3`.

## 3. Disk

Identify the spare disk and be sure:

```
lsblk -o NAME,SIZE,MODEL,SERIAL,FSTYPE,MOUNTPOINTS
ls -l /dev/disk/by-id/ | grep -v part
```

Expect the spare disk with no FSTYPE and no mount point. If anything is
mounted from it, stop. Then (this destroys what is on it):

```
parted -s /dev/sdX mklabel gpt mkpart trial ext4 1MiB 100%
mkfs.ext4 -L trial /dev/sdX1
blkid -s UUID -o value /dev/sdX1         # copy this UUID into the fstab line below
```

Add to `/etc/fstab` (the raw mount first, then dcfs on the consumers'
path; `UUID=` is the one printed above):

```
UUID=<uuid-from-blkid>  /srv/raw/trial  ext4  noatime,nofail  0 2
/srv/raw/trial          /srv/trial      dcfs  dcfs.fstype=bind,x-systemd.requires-mounts-for=/srv/raw/trial,dcfs.cache_db=/var/lib/dcfs/trial.db,dcfs.stderrthreshold=0,nofail  0 0
```

`dcfs.stderrthreshold=0` makes the daemon log its INFO lines (sync points,
"first backing access after N idle"), which the trial needs; without it
only warnings are logged. The sixth field of the dcfs line is `0` on
purpose: a `bind` line has no device to check (README). The cache database
sits on the always-on root disk (`/var/lib/dcfs`).

```
mkdir -p /srv/raw/trial /srv/trial /var/lib/dcfs
systemctl daemon-reload
mount /srv/raw/trial
findmnt /srv/raw/trial                   # expect ext4, options including noatime
```

Put a tree on the disk now, through the raw path, while dcfs is not
mounted (a copy of something real with many files and directories, tens of
thousands at least, is what makes the trial meaningful):

```
rsync -a /some/real/tree/ /srv/raw/trial/
sync
pgrep ext4lazyinit || echo "lazy init finished"
```

(Lazy inode-table initialisation after mkfs writes in the background and
would spin the disk later; wait for it to finish; `pgrep` prints a pid
until it has.) Then:

```
mount /srv/trial                         # expect: no output, exit 0
findmnt /srv/trial                       # expect: source /srv/raw/trial, FSTYPE fuse.dcfs
findmnt /sys/fs/fuse/connections         # expect: fusectl (section 1)
ps -eo pid,comm,args | grep '[d]cfs'     # find the daemon; note its pid
journalctl -t dcfs -n 20 --no-pager      # expect the start line and recovery summary
```

In your root shell set the variables the watcher uses (type the daemon's
pid from the `ps` line):

```
D=/dev/sdX
P=<daemon pid>
```

Keep the raw disk quiet from here on: no `ls`, `df` or `find` on
`/srv/raw`. Stop things that poll or crawl it on their own for the trial:
`systemctl stop smartd plocate-updatedb.timer mlocate.timer 2>/dev/null`
(unit names vary; ignore "not loaded"), and quit file managers.

## 4. Warm

```
time find /srv/trial -ls > /dev/null     # first, cold listing: reads the disk, slow on a big tree
time find /srv/trial -ls > /dev/null     # second: expect much faster; the disk is not read
awk '{print "daemon cpu_s=" ($14+$15)/100}' /proc/$P/stat
```

Record both times and the CPU seconds. Confirm the cache has rows
(read-only query; the daemon keeps running):

```
sqlite3 -readonly /var/lib/dcfs/trial.db 'select count(*) from inodes; select count(*) from dentries;'
```

Expect counts near the number of objects in the tree (`find /srv/trial |
wc -l`). Note `ls -l /var/lib/dcfs/trial.db*` too.

## 5. Spin down and watch

```
hdparm -y $D                             # expect: issuing standby command
hdparm -C $D                             # expect: drive state is:  standby
```

If `hdparm` says the command is unsupported (some USB bridges), use
`smartctl -n standby -i $D`; in standby it prints `Device is in STANDBY
mode, exit(2)` and does not wake the disk; awake it prints the identity.

The watcher: a line you type, in terminal 1, every few minutes (up-arrow,
enter; no loop, no script):

```
echo "$(date +%T) $(hdparm -C $D | awk '/state/{print $NF}') cpu_s=$(awk '{print ($14+$15)/100}' /proc/$P/stat)"
```

Expect `HH:MM:SS standby cpu_s=N.NN`; any other state (`active/idle`) is a
spin-up. Type it now, once, as the baseline, before any work. Terminal 3:

```
journalctl -t dcfs -f
```

The lines to watch: `sync point: cleared N of M dirty rows in T` (a
sync point, with `syncfs` of the backing filesystem), and `first backing
access after Ns idle (...)` (the daemon read the disk after more than 60
seconds without; the parenthesis says what for). Both are INFO lines,
hence the option in the fstab line. Sync points happen only on a request
after dirty rows exist; zero is a possible and good count.

## 6. The hour of metadata work

In terminal 2, spread over an hour; run the watcher after each step, at
least every five minutes in between, and note the time and the command
running whenever the state is not `standby`:

```
ls -R /srv/trial > /dev/null
find /srv/trial -ls > /dev/null
find /srv/trial -type f -print0 | xargs -0 -n 5000 stat > /dev/null
find /srv/trial -type d -print0 | xargs -0 -n 5000 stat -f > /dev/null
df /srv/trial; df -i /srv/trial
updatedb -U /srv/trial -o /tmp/trial.plocate && locate -d /tmp/trial.plocate -c ''
```

`updatedb` (plocate or mlocate; skip if locate is not installed) normally
skips FUSE filesystems through `PRUNEFS` in `/etc/updatedb.conf`; if the
count printed is 0, remove `fuse` from `PRUNEFS` for the trial and rerun,
and say so in the report. Nothing in the list opens a file; do not run
`grep -r`, `file`, `md5sum`, thumbnailers or an
indexer over the mount: opening a file spins the disk up (section 7).

NFS from another machine, if you export it. On the host:

```
apt install nfs-kernel-server
echo '/srv/trial  192.168.1.0/24(ro,fsid=1,no_subtree_check)' >> /etc/exports
exportfs -ra
exportfs -v                              # expect /srv/trial with fsid=1
```

(`fsid=` is required for a FUSE filesystem; use your own subnet. After any
unmount and mount of dcfs, run `exportfs -f`.) On the other machine:

```
mkdir -p /mnt/trial && mount -t nfs -o ro,vers=4.2 HOST:/srv/trial /mnt/trial
ls -R /mnt/trial > /dev/null
find /mnt/trial -ls > /dev/null
```

Record, for the hour:
- every spin-up: wall time, the command that was running;
- daemon CPU seconds at the start and the end (the watcher's `cpu_s`);
- the number of sync points and the times they fired relative to the
  spin-down: `journalctl -t dcfs --since '70 minutes ago' --no-pager | grep -c 'sync point'`
  and `... | grep 'sync point'` for the times;
- every `first backing access after` line.

The disk must stay in `standby` throughout. Run `hdparm -C $D` one last
time at the end.

## 7. A read

```
head -c 1M "$(find /srv/trial -type f -size +1M -print -quit)" > /dev/null
hdparm -C $D                             # expect: drive state is:  active/idle
```

Expected: the disk spun up (a read opens the file on the backing disk), and
the journal has `first backing access after ...`. Let the disk spin down
by its own timer (`hdparm -S` if you set one) or with `hdparm -y $D`, check
`standby`, then repeat a short version of section 6 (`ls -R`, `find -ls`,
`stat` of a few thousand files, `df`; 10 to 15 minutes, with the watcher).

## 8. Unmount and remount (warm restart)

```
umount /srv/trial                        # expect: no output, exit 0; returns once the daemon has exited
findmnt /srv/trial                       # expect: nothing
ps -eo pid,comm | grep '[d]cfs' || echo "daemon gone"
journalctl -t dcfs -n 5 --no-pager       # expect: shutdown: clean (...)
fsck.dcfs -n -o dcfs.fstype=bind,dcfs.cache_db=/var/lib/dcfs/trial.db /srv/raw/trial   # optional: expect no errors
exportfs -f                              # only if exporting
mount /srv/trial
ps -eo pid,comm,args | grep '[d]cfs'     # the new daemon; set P=<its pid>
journalctl -t dcfs -n 20 --no-pager      # expect recovery: the last run ended cleanly
```

The disk must still be in standby if nothing read it. Repeat a short
version of section 6 (10 to 15 minutes, watcher as before) and
confirm the first `find /srv/trial -ls` is fast (the cache survived).

If `umount` returned before the daemon was gone, or `mount` complains
"in use by another dcfs process", record it with the output of
`findmnt /sys/fs/fuse/connections`.

## 9. The control (optional)

What dcfs buys over the kernel's own caches. Unmount dcfs and work on the
raw path with caches warmed and never reclaimed:

```
umount /srv/trial
sysctl vm.vfs_cache_pressure                       # note the value (default 100)
sysctl -w vm.vfs_cache_pressure=0
find /srv/raw/trial -ls > /dev/null                # warms the kernel's caches: spins the disk up
hdparm -y $D; hdparm -C $D                         # expect: standby
```

Then the section 6 workload on `/srv/raw/trial` for the hour, with the
watcher (no daemon: drop the `cpu_s` part). Does the disk stay down? Then:

```
sysctl -w vm.vfs_cache_pressure=100                # or the value noted above
mount /srv/trial
```

`vfs_cache_pressure=0` never reclaims dentries and inodes and can exhaust
memory on a big tree; fine on this host with 62 GB, and the reason it does
not substitute for dcfs on a RAM-starved server (the main fileserver).
Record whether the disk stayed down and, if not, when it woke.

## 10. What to send back

- The recorded lines: the watcher output from each section, the times of
  `find` in section 4, the spin-ups with the command running.
- `journalctl -t dcfs --no-pager` for the whole trial.
- `ls -l /var/lib/dcfs/trial.db*` (the cache database size) and the two row
  counts from section 4.
- The kernel (`uname -r`), `dcfs --version`, the disk model, the tree's
  object count.
- One line per section, pass or fail: 1 kernel, 2 build and install, 3
  disk, 4 warm, 5 watch, 6 hour, 7 read, 8 remount, 9 control.

A spin-up during metadata-only work (sections 6 and 8) is a bug, not a
result to explain away: send the time and the command, and it is
reproduced in QEMU as a failing test first (Phase 10's idle test or a new
one) before anything is fixed. A spin-up with a `first backing access
after` line names the request that reached the disk (the parenthesis);
one with no such line came from outside dcfs (the kernel, another
process) and is worth knowing too.
