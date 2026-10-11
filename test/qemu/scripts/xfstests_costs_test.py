# SPDX-License-Identifier: MIT
"""Tests for xfstests_costs.py (step 26.17): the per-test cost lines of a dcfs
run and of a native run (xfstests_native=1), put side by side."""

import os
import subprocess
import sys
import tempfile
import unittest

import xfstests_costs

# What guest/xfstests.sh prints per test: wall time, the CPU of the processes
# that run dcfs's code (the daemon), and the disks' I/O between the test's end
# and the previous one's.
DCFS = """\
xfstests.sh: shard 1/6: 4 tests, limit 200s each
xfstests: generic/001 pass 12s dcfs-cpu=2.500s reads=10 writes=40 sectors_read=80 sectors_written=900 flushes=30
xfstests: generic/002 pass 3s dcfs-cpu=0.100s reads=1 writes=5 sectors_read=8 sectors_written=40 flushes=2
xfstests: generic/003 notrun 1s dcfs-cpu=0.000s reads=0 writes=0 sectors_read=0 sectors_written=0 flushes=0: this test requires a block device
xfstests: generic/004 fail 90s dcfs-cpu=60.000s reads=500 writes=4000 sectors_read=4000 sectors_written=64000 flushes=1500
xfstests: generic/005 pass 5s dcfs-cpu=1.000s reads=2 writes=9 sectors_read=16 sectors_written=70 flushes=4
"""
NATIVE = """\
xfstests: generic/001 pass 2s dcfs-cpu=0.000s reads=10 writes=35 sectors_read=80 sectors_written=880 flushes=8
xfstests: generic/002 pass 3s dcfs-cpu=0.000s reads=1 writes=5 sectors_read=8 sectors_written=40 flushes=2
xfstests: generic/003 notrun 1s dcfs-cpu=0.000s reads=0 writes=0 sectors_read=0 sectors_written=0 flushes=0: this test requires a block device
xfstests: generic/004 pass 10s dcfs-cpu=0.000s reads=500 writes=3900 sectors_read=4000 sectors_written=63000 flushes=100
"""


class ParseTest(unittest.TestCase):

    def test_a_line_has_its_numbers(self):
        got = xfstests_costs.parse(DCFS)
        self.assertEqual(list(got), ['generic/001', 'generic/002', 'generic/003',
                                     'generic/004', 'generic/005'])
        self.assertEqual(got['generic/001'], dict(
            status='pass', wall=12, cpu=2.5, reads=10, writes=40,
            sectors_read=80, sectors_written=900, flushes=30))
        self.assertEqual(got['generic/004']['status'], 'fail')
        self.assertEqual(got['generic/003']['status'], 'notrun')

    def test_other_lines_are_not_tests(self):
        self.assertEqual(xfstests_costs.parse('xfstests.sh: shard 1/6\n'), {})

    def test_a_log_from_before_the_cost_lines_is_refused(self):
        old = 'xfstests: generic/001 pass 12s dcfs-cpu=40ticks\n'
        with self.assertRaises(xfstests_costs.NoCosts):
            xfstests_costs.compare(old, NATIVE)
        with self.assertRaises(xfstests_costs.NoCosts):
            xfstests_costs.compare(DCFS, '')


class CompareTest(unittest.TestCase):

    def setUp(self):
        self.rows = xfstests_costs.compare(DCFS, NATIVE)

    def test_the_slowest_extra_time_comes_first(self):
        self.assertEqual([r['test'] for r in self.rows],
                         ['generic/004', 'generic/001', 'generic/002',
                          'generic/003', 'generic/005'])

    def test_extra_time_cpu_and_the_rest(self):
        r = {x['test']: x for x in self.rows}
        # 90 s against 10 s native: 80 s more, 60 s of it the daemon computing,
        # 20 s of it waiting for something else (I/O, the kernel).
        self.assertEqual((r['generic/004']['extra'], r['generic/004']['cpu'],
                          r['generic/004']['waiting']), (80, 60.0, 20.0))
        self.assertEqual(r['generic/001']['extra'], 10)
        self.assertEqual(r['generic/001']['extra_writes'], 5)
        self.assertEqual(r['generic/001']['extra_flushes'], 22)
        self.assertEqual(r['generic/001']['extra_sectors_written'], 20)
        self.assertEqual(r['generic/004']['extra_flushes'], 1400)

    def test_a_test_native_did_not_run_has_no_comparison(self):
        r = {x['test']: x for x in self.rows}
        self.assertIsNone(r['generic/005']['extra'])
        self.assertEqual(r['generic/005']['cpu'], 1.0)

    def test_a_test_that_is_faster_through_dcfs_is_not_negative_waiting(self):
        rows = xfstests_costs.compare(
            'xfstests: generic/001 pass 1s dcfs-cpu=0.500s reads=0 writes=0 '
            'sectors_read=0 sectors_written=0 flushes=0\n',
            'xfstests: generic/001 pass 4s dcfs-cpu=0.000s reads=0 writes=0 '
            'sectors_read=0 sectors_written=0 flushes=0\n')
        self.assertEqual(rows[0]['extra'], -3)
        self.assertEqual(rows[0]['waiting'], -3.5)


class CommandLineTest(unittest.TestCase):

    def test_the_table(self):
        with tempfile.TemporaryDirectory() as tmp:
            d, n = os.path.join(tmp, 'dcfs.log'), os.path.join(tmp, 'native.log')
            for path, text in ((d, DCFS), (n, NATIVE)):
                with open(path, 'w') as f:
                    f.write(text)
            out = subprocess.run(
                [sys.executable, '-I', xfstests_costs.__file__, d, n, '--top', '2'],
                capture_output=True, text=True, check=True).stdout.splitlines()
        self.assertEqual(len(out), 3)  # header and two tests
        self.assertTrue(out[0].split()[0] == 'test')
        self.assertTrue(out[1].startswith('generic/004'), out[1])
        self.assertTrue(out[2].startswith('generic/001'), out[2])


if __name__ == '__main__':
    unittest.main()
