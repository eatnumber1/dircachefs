#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Print Claude Code's /usage panel as plain text (step 26.29).

Drives an interactive `claude` in a pseudo-terminal and prints the panel it
draws for /usage. The figures are approximate: they come from this machine's
local sessions only, not from the account.

The checkout claude runs in must already be trusted by claude, so that no
trust dialog appears; the default is the current directory. Run it from the
repository root, with a logged-in `claude` on PATH:

    timeout 90 python3 -I tools/agents/claude_usage.py

The hard bound is 90 s (the timeout above). The script exits 1 if the panel
never appears, and kills claude before it exits.
"""

import argparse
import os
import pty
import re
import select
import signal
import sys
import time

# The deadline for the panel, inside the 90 s bound of the command above.
DEADLINE = 60.0
# Cursor-forward moves, which become spaces.
CSI = re.compile(rb"\x1b\[(\d*)C")
# Cursor-position moves, which become a space.
CURSOR_POSITION = re.compile(rb"\x1b\[[\d;]*[Hf]")
ANSI = re.compile(
    rb"\x1b\[[0-?]*[ -/]*[@-~]|\x1b\][^\x07]*(\x07|\x1b\\)|\x1b[@-Z\\-_]"
    rb"|\x1b[()][0-9A-Za-z]"
)
# The input prompt glyph, U+276F (a heavy right-pointing angle quotation mark).
PROMPT_GLYPH = b"\xe2\x9d\xaf"
# The placeholder text of the empty input line.
PROMPT_PLACEHOLDER = b"Try"
PANEL_FOOTER = re.compile(r"What'?s\s*contributing")
PANEL_HEADER = re.compile(r"Current\s*session")


def read_for(fd, secs, buf):
    # 1.11: a bounded wait, the one kind of wait a pty allows. It has no event
    # for "the prompt is up" or "the panel has drawn", so the callers poll
    # their condition between these reads, up to a deadline. The 0.2 s select
    # only bounds each poll.
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


def prompt_seen(buf):
    return PROMPT_PLACEHOLDER in buf or PROMPT_GLYPH in buf


def panel_done(buf):
    # The footer is the last thing the panel draws. Spacing in the raw bytes
    # is cursor moves, so match on the cleaned text.
    return PANEL_FOOTER.search(clean(buf)) is not None


def clean(buf):
    buf = CSI.sub(lambda m: b" " * int(m.group(1) or 1), buf)
    buf = CURSOR_POSITION.sub(b" ", buf)
    return ANSI.sub(b"", buf).decode("utf-8", "replace")


def extract_panel(text):
    """The panel's lines, whitespace collapsed; None if it is not in text."""
    footers = list(PANEL_FOOTER.finditer(text))
    headers = [
        m
        for m in PANEL_HEADER.finditer(text)
        if footers and m.start() < footers[-1].start()
    ]
    if not footers or not headers:
        return None
    panel = text[headers[-1].start() : footers[-1].start()]
    lines = [" ".join(line.split()) for line in panel.splitlines()]
    return "\n".join(line for line in lines if line)


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--cwd",
        default=os.getcwd(),
        help=(
            "the checkout claude runs in; claude must already trust it, so "
            "that no trust dialog appears (default: the current directory)"
        ),
    )
    return parser.parse_args(argv)


def main(argv):
    args = parse_args(argv)
    start = time.time()
    pid, fd = pty.fork()
    if pid == 0:
        os.chdir(args.cwd)
        os.execvp("claude", ["claude"])
    buf = b""
    try:
        # Wait for the input prompt, up to 20 s.
        while time.time() < start + 20 and not prompt_seen(buf):
            buf = read_for(fd, 0.5, buf)
        # The prompt can be drawn before claude reads input, and nothing
        # signals that it does; 1.0 s bounds that gap.
        buf = read_for(fd, 1.0, buf)
        os.write(fd, b"/usage\r")
        while time.time() - start < DEADLINE and not panel_done(buf):
            buf = read_for(fd, 0.5, buf)
    finally:
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        os.waitpid(pid, 0)
    panel = extract_panel(clean(buf))
    if panel is None:
        sys.stderr.write("claude_usage: usage panel not found\n")
        return 1
    print(panel)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
