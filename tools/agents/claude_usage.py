#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Print Claude Code's /usage panel as plain text (step 26.29).

Drives an interactive `claude` in a pseudo-terminal and prints the panel it
draws for /usage. The figures are approximate: they come from this machine's
local sessions only, not from the account. Run it from the repository root,
with a logged-in `claude` on PATH:

    timeout 90 python3 -I tools/agents/claude_usage.py

It runs claude in an already-trusted checkout (no trust prompt, no config
writes). The hard bound is 90 s; it exits 1 if the panel never appears, and
kills claude before it exits.
"""

import os
import pty
import re
import select
import signal
import sys
import time

# Already trusted by claude, so no trust dialog appears.
CWD = "/home/russ/Sources/dircachefs"
# Inside the 90 s shell bound.
DEADLINE = 60.0
# Cursor-forward moves become spaces.
CSI = re.compile(rb"\x1b\[(\d*)C")
ANSI = re.compile(
    rb"\x1b\[[0-?]*[ -/]*[@-~]|\x1b\][^\x07]*(\x07|\x1b\\)|\x1b[@-Z\\-_]"
    rb"|\x1b[()][0-9A-Za-z]"
)


def read_for(fd, secs, buf):
    # 1.11: a bounded read of the pty. There is no event for "the panel has
    # drawn" or "the prompt is up" on a pty, so the waits below poll their
    # condition on a deadline; the 0.2 s select only bounds each poll.
    end = time.time() + secs
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.2)
        if r:
            try:
                d = os.read(fd, 65536)
            except OSError:
                break
            if not d:
                break
            buf += d
    return buf


def panel_done(buf):
    # The panel is done when its "What's contributing" footer has drawn.
    # Spacing in the raw bytes is cursor moves, so match on the cleaned text
    # with whitespace ignored.
    return re.search(r"What'?s\s*contributing", clean(buf)) is not None


def clean(buf):
    buf = CSI.sub(lambda m: b" " * int(m.group(1) or 1), buf)
    # Cursor-position moves become a space.
    buf = re.sub(rb"\x1b\[[\d;]*[Hf]", b" ", buf)
    return ANSI.sub(b"", buf).decode("utf-8", "replace")


def main():
    start = time.time()
    pid, fd = pty.fork()
    if pid == 0:
        os.chdir(CWD)
        os.execvp("claude", ["claude"])
    buf = b""
    try:
        # Wait for the input prompt (the placeholder "Try" line or U+276F).
        ready = start + 20
        while (
            time.time() < ready
            and b"Try" not in buf
            and "❯".encode() not in buf
        ):
            buf = read_for(fd, 0.5, buf)
        buf = read_for(fd, 1.0, buf)
        os.write(fd, b"/usage\r")
        while time.time() - start < DEADLINE and not panel_done(buf):
            buf = read_for(fd, 0.5, buf)
        # Let the panel finish drawing.
        buf = read_for(fd, 1.5, buf)
        text = clean(buf)
    finally:
        try:
            os.write(fd, b"\x1b")
            os.write(fd, b"/exit\r")
        except OSError:
            pass
        read_for(fd, 0.5, b"")
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        os.waitpid(pid, 0)
    # Keep only the usage panel.
    footers = list(re.finditer(r"What'?s\s*contributing", text))
    sessions = [
        m
        for m in re.finditer(r"Current\s*session", text)
        if footers and m.start() < footers[-1].start()
    ]
    if not footers or not sessions:
        sys.stderr.write("claude_usage: usage panel not found\n")
        return 1
    panel = text[sessions[-1].start() : footers[-1].start()]
    lines = [" ".join(line.split()) for line in panel.splitlines()]
    lines = [line for line in lines if line]
    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
