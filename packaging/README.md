# dcfs systemd unit

Install `dcfs.service` to `/etc/systemd/system/`, copy `dcfs.env.example`
to `/etc/dcfs/dcfs.env` and fill in `SOURCE`/`CACHE_DB`/`MOUNTPOINT`, then:

```
systemctl daemon-reload
systemctl enable --now dcfs
```

dcfs runs as root (`User=root`): it needs `CAP_DAC_READ_SEARCH` (for
`open_by_handle_at`, used to reopen cached inodes by handle) and
`CAP_SYS_ADMIN` (for mounting the FUSE filesystem and FUSE passthrough on
the backing files). Running as root is simpler than assembling the
equivalent capability set, so that's what the unit does.

`Restart=on-failure` restarts dcfs if it exits nonzero (e.g. a crash);
`fuse_set_signal_handlers` makes a normal `systemctl stop` (SIGTERM) exit
cleanly instead, so it isn't treated as a failure.
