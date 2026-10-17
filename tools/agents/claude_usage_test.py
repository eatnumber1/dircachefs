# SPDX-License-Identifier: MIT
"""Tests for claude_usage: the screen cleaning and the panel extraction.

The pty driver itself is not tested: it needs a logged-in claude CLI.
"""

import unittest

import claude_usage

# A synthetic capture, not a recording of claude: cursor-forward moves
# (ESC [ n C) stand where claude's panel puts its spacing.
FIXTURE = (
    b"\x1b[2J\x1b[H"
    b"Current\x1b[1Csession\x1b[3C12%\r\n"
    b"Resets\x1b[1C2pm\r\n"
    b"\x1b[31mWeekly\x1b[0m\x1b[1Cusage\x1b[2C3%\r\n"
    b"What's\x1b[1Ccontributing\r\n"
)


class CleanTest(unittest.TestCase):

    def test_cursor_forward_becomes_spaces(self):
        text = claude_usage.clean(b"a\x1b[3Cb")
        self.assertEqual(text, "a   b")

    def test_cursor_position_and_colour_become_space_and_nothing(self):
        text = claude_usage.clean(b"a\x1b[2;5Hb\x1b[31mc\x1b[0m")
        self.assertEqual(text, "a bc")


class PanelDoneTest(unittest.TestCase):

    def test_not_done_before_the_footer(self):
        self.assertFalse(claude_usage.panel_done(FIXTURE[:40]))

    def test_done_once_the_footer_has_drawn(self):
        self.assertTrue(claude_usage.panel_done(FIXTURE))


class ExtractPanelTest(unittest.TestCase):

    def test_keeps_the_panel_with_collapsed_whitespace(self):
        panel = claude_usage.extract_panel(claude_usage.clean(FIXTURE))
        self.assertEqual(
            panel,
            "Current session 12%\nResets 2pm\nWeekly usage 3%",
        )

    def test_uses_the_last_panel_drawn(self):
        text = claude_usage.clean(FIXTURE + FIXTURE)
        self.assertEqual(
            claude_usage.extract_panel(text).count("Current session"), 1
        )

    def test_none_without_the_footer(self):
        text = claude_usage.clean(FIXTURE[: FIXTURE.index(b"What")])
        self.assertIsNone(claude_usage.extract_panel(text))

    def test_none_without_the_header_before_the_footer(self):
        self.assertIsNone(
            claude_usage.extract_panel("What's contributing\nCurrent session")
        )


if __name__ == "__main__":
    unittest.main()
