# SPDX-License-Identifier: MIT
"""Where do the slow xfstests cases spend their time? (step 26.17, for 17.3)

guest/xfstests.sh prints a line per test:

    xfstests: generic/001 pass 12s dcfs-cpu=2.500s reads=10 writes=40 sectors_read=80 sectors_written=900 flushes=30

the wall time, the CPU of the processes that run dcfs's code (the daemon: its
cgroup, exact, exited daemons included), and the I/O of the disks (reads and
writes completed, sectors, flush requests: what an fsync costs the device)
between the previous test's end and this one's. The same shard run natively
(xfstests_native=1, no dcfs) prints the same lines with no daemon CPU. This puts
the two side by side, the tests dcfs slows most first:

    xfstests_costs.py DCFS_SERIAL_LOG NATIVE_SERIAL_LOG [--top N]

`extra` is the seconds dcfs added to the test, `cpu` the daemon's CPU for it,
and `waiting` the rest of the extra time: what the daemon spent waiting for the
disk, the kernel or the test, not computing. The `extra_` counts are the writes,
flushes and sectors written dcfs added; a test whose extra time is mostly
waiting and whose extra flushes are many is paying for fsyncs.
"""

import argparse
import re
import sys

LINE = re.compile(
    r'^xfstests: (generic/\d+) (\w+) (\d+)s dcfs-cpu=([0-9.]+)s '
    r'reads=(\d+) writes=(\d+) sectors_read=(\d+) sectors_written=(\d+) '
    r'flushes=(\d+)(?:: .*)?$')


class NoCosts(Exception):
    """A log with no per-test cost lines (one from before step 26.17, or not an
    xfstests log)."""


def parse(text):
    """{test: numbers} for the cost lines of a serial log, in order of
    appearance (a test that ran twice, after a retry, keeps its last line)."""
    tests = {}
    for line in text.splitlines():
        m = LINE.match(line.rstrip('\r'))
        if m:
            tests[m.group(1)] = dict(
                status=m.group(2), wall=int(m.group(3)), cpu=float(m.group(4)),
                reads=int(m.group(5)), writes=int(m.group(6)),
                sectors_read=int(m.group(7)),
                sectors_written=int(m.group(8)), flushes=int(m.group(9)))
    return tests


def compare(dcfs_text, native_text):
    """Rows for the tests of the dcfs run, the largest extra time first; a test
    the native run has no line for has no extra."""
    dcfs, native = parse(dcfs_text), parse(native_text)
    if not dcfs or not native:
        raise NoCosts(
            'no per-test cost lines in the %s log (lines like "xfstests: '
            'generic/001 pass 12s dcfs-cpu=2.500s reads=10 writes=40 ..."; a '
            'log from before step 26.17 has dcfs-cpu=40ticks)'
            % ('dcfs' if not dcfs else 'native'))
    rows = []
    for test, d in dcfs.items():
        n = native.get(test)
        row = dict(test=test, status=d['status'], wall=d['wall'], cpu=d['cpu'],
                   native_wall=None, extra=None, waiting=None,
                   extra_writes=None, extra_flushes=None,
                   extra_sectors_written=None)
        if n:
            row.update(
                native_wall=n['wall'], extra=d['wall'] - n['wall'],
                waiting=d['wall'] - n['wall'] - d['cpu'],
                extra_writes=d['writes'] - n['writes'],
                extra_flushes=d['flushes'] - n['flushes'],
                extra_sectors_written=d['sectors_written'] - n['sectors_written'])
        rows.append(row)
    rows.sort(key=lambda r: (r['extra'] is None, -(r['extra'] or 0), r['test']))
    return rows


def table(rows):
    """The rows as aligned text, header first."""
    def cell(v, fmt='%d'):
        return '-' if v is None else fmt % v

    head = ['test', 'status', 'dcfs_s', 'native_s', 'extra_s', 'cpu_s',
            'waiting_s', 'extra_writes', 'extra_flushes', 'extra_sectors_written']
    body = [[r['test'], r['status'], cell(r['wall']), cell(r['native_wall']),
             cell(r['extra']), cell(r['cpu'], '%.1f'),
             cell(r['waiting'], '%.1f'), cell(r['extra_writes']),
             cell(r['extra_flushes']), cell(r['extra_sectors_written'])]
            for r in rows]
    widths = [max(len(x[i]) for x in [head] + body) for i in range(len(head))]
    return '\n'.join('  '.join(c.ljust(w) for c, w in zip(line, widths)).rstrip()
                     for line in [head] + body)


def main(argv):
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    p.add_argument('dcfs_log')
    p.add_argument('native_log')
    p.add_argument('--top', type=int, default=0,
                   help='only the N tests dcfs slows most (default: all)')
    args = p.parse_args(argv)
    with open(args.dcfs_log, errors='replace') as f:
        dcfs = f.read()
    with open(args.native_log, errors='replace') as f:
        native = f.read()
    try:
        rows = compare(dcfs, native)
    except NoCosts as e:
        print('xfstests_costs.py: %s' % e, file=sys.stderr)
        return 1
    print(table(rows[:args.top] if args.top else rows))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
