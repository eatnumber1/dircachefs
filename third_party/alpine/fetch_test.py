"""Tests of apk.py's download step against a fake mirror (step 24.1, M1).

The mirror drops superseded builds within about a week, so a package
repository fetched against an old index snapshot meets a 404. The failure
must say what to do, not just "404".
"""

import http.server
import io
import json
import os
import sys
import tempfile
import threading
import unittest

import apk


class Mirror(http.server.BaseHTTPRequestHandler):
    """Serves one package of branch v3.99 and 404s everything else."""

    def do_GET(self):
        if self.path == '/v3.99/main/x86_64/present-1.0-r0.apk':
            body = b'apk bytes'
            self.send_response(200)
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_response(404)
            self.end_headers()

    def log_message(self, *args):
        pass


class DownloadTest(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.server = http.server.HTTPServer(('127.0.0.1', 0), Mirror)
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()
        cls.mirror = f'http://127.0.0.1:{cls.server.server_port}'

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()

    def run_download(self, files):
        with tempfile.TemporaryDirectory() as tmp:
            resolved = os.path.join(tmp, 'resolved.json')
            with open(resolved, 'w') as f:
                json.dump([{'file': name, 'repo': 'main', 'branch': 'v3.99'}
                           for name in files], f)
            stderr, old = io.StringIO(), sys.stderr
            sys.stderr = stderr
            try:
                rc = apk.main([
                    'download', '--resolved', resolved, '--mirror',
                    self.mirror, '--arch', 'x86_64', '--out',
                    os.path.join(tmp, 'apks')
                ])
            finally:
                sys.stderr = old
            got = {}
            apks = os.path.join(tmp, 'apks')
            if os.path.isdir(apks):
                for name in os.listdir(apks):
                    with open(os.path.join(apks, name), 'rb') as f:
                        got[name] = f.read()
            return rc, stderr.getvalue(), got

    def test_present_package_is_downloaded(self):
        rc, _, got = self.run_download(['present-1.0-r0.apk'])
        self.assertEqual(rc, 0)
        self.assertEqual(got, {'present-1.0-r0.apk': b'apk bytes'})

    def test_a_404_says_to_refetch_the_index(self):
        rc, err, _ = self.run_download(
            ['present-1.0-r0.apk', 'gone-2.0-r1.apk'])
        self.assertEqual(rc, 1)
        self.assertIn('gone-2.0-r1.apk', err)
        self.assertIn(
            'the index snapshot names a build the mirror no longer serves; '
            'run `bazel fetch --force --repo=@alpine_index`', err)

    def test_an_unreachable_mirror_is_not_blamed_on_the_index(self):
        stderr, old = io.StringIO(), sys.stderr
        sys.stderr = stderr
        try:
            with tempfile.TemporaryDirectory() as tmp:
                resolved = os.path.join(tmp, 'r.json')
                with open(resolved, 'w') as f:
                    json.dump([{'file': 'a.apk', 'repo': 'main',
                                'branch': 'v3.99'}], f)
                rc = apk.main(['download', '--resolved', resolved,
                               '--mirror', 'http://127.0.0.1:1', '--arch',
                               'x86_64', '--out', os.path.join(tmp, 'o')])
        finally:
            sys.stderr = old
        self.assertEqual(rc, 1)
        self.assertNotIn('bazel fetch --force', stderr.getvalue())


if __name__ == '__main__':
    unittest.main()
