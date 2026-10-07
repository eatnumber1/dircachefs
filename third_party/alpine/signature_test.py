"""Tests that the Alpine package checks refuse what they must (step 24.1).

A tampered APKINDEX and a tampered .apk are refused (signature, control
checksum and data hash), a package missing from the branch fails with a
message naming it, and one real, signed Alpine package verifies against the
checked-in keys, so the format and the keys are what Alpine really uses. The
other cases use packages signed here with a throwaway RSA key (generated at
run time; only the public half is ever given to apk.py).
"""

import base64
import gzip
import hashlib
import io
import json
import os
import random
import sys
import tarfile
import tempfile
import unittest

import apk

KEY_FILE = 'test@example.org-00000000.rsa.pub'


def _is_probable_prime(n, rng):
    if n < 4 or n % 2 == 0:
        return n in (2, 3)
    d, r = n - 1, 0
    while d % 2 == 0:
        d //= 2
        r += 1
    for _ in range(20):
        x = pow(rng.randrange(2, n - 1), d, n)
        if x in (1, n - 1):
            continue
        for _ in range(r - 1):
            x = pow(x, 2, n)
            if x == n - 1:
                break
        else:
            return False
    return True


def _random_prime(bits, rng):
    while True:
        n = rng.getrandbits(bits) | (1 << (bits - 1)) | 1
        if _is_probable_prime(n, rng):
            return n


def _der(tag, content):
    if len(content) < 0x80:
        length = bytes([len(content)])
    else:
        raw = len(content).to_bytes((len(content).bit_length() + 7) // 8,
                                    'big')
        length = bytes([0x80 | len(raw)]) + raw
    return bytes([tag]) + length + content


def _der_int(n):
    raw = n.to_bytes((n.bit_length() + 8) // 8, 'big')
    return _der(0x02, raw)


class TestKey:
    """A 1024-bit RSA key pair for tests (not secret, not for real use)."""

    def __init__(self):
        rng = random.Random(24)
        while True:
            p, q = _random_prime(512, rng), _random_prime(512, rng)
            e = 65537
            phi = (p - 1) * (q - 1)
            if p != q and phi % e != 0:
                break
        self.n = p * q
        self.d = pow(e, -1, phi)
        rsa_key = _der(0x30, _der_int(self.n) + _der_int(e))
        algorithm = _der(0x30, _der(0x06, bytes.fromhex('2a864886f70d010101'))
                         + _der(0x05, b''))
        spki = _der(0x30, algorithm + _der(0x03, b'\x00' + rsa_key))
        b64 = base64.b64encode(spki).decode('ascii')
        self.pem = ('-----BEGIN PUBLIC KEY-----\n' + b64 +
                    '\n-----END PUBLIC KEY-----\n')

    def sign(self, signed):
        size = (self.n.bit_length() + 7) // 8
        digest = (bytes.fromhex('3021300906052b0e03021a05000414') +
                  hashlib.sha1(signed).digest())
        padded = (b'\x00\x01' + b'\xff' * (size - len(digest) - 3) + b'\x00' +
                  digest)
        return pow(int.from_bytes(padded, 'big'), self.d,
                   self.n).to_bytes(size, 'big')


KEY = TestKey()


def _tar_gz(members):
    """Returns a gzip stream holding a tar of {name: bytes}."""
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode='w') as archive:
        for name, content in members.items():
            info = tarfile.TarInfo(name)
            info.size = len(content)
            archive.addfile(info, io.BytesIO(content))
    return gzip.compress(buf.getvalue(), mtime=0)


def _signature(signed):
    return _tar_gz({f'.SIGN.RSA.{KEY_FILE}': KEY.sign(signed)})


def make_apk(files=None):
    """Returns the bytes of an .apk signed with the test key."""
    data = _tar_gz(files or {'usr/share/hello': b'hello\n'})
    pkginfo = (f'pkgname = hello\ndatahash = '
               f'{hashlib.sha256(data).hexdigest()}\n').encode('ascii')
    control = _tar_gz({'.PKGINFO': pkginfo})
    return _signature(control) + control + data


def make_index(records):
    """Returns the bytes of an APKINDEX.tar.gz signed with the test key."""
    text = '\n\n'.join('\n'.join(f'{k}:{v}' for k, v in r.items())
                       for r in records) + '\n\n'
    body = _tar_gz({'DESCRIPTION': b'test', 'APKINDEX': text.encode('utf-8')})
    return _signature(body) + body


def flip(data, offset):
    return data[:offset] + bytes([data[offset] ^ 1]) + data[offset + 1:]


class IndexTest(unittest.TestCase):

    def setUp(self):
        self.keys = {KEY_FILE: KEY.pem}
        self.records = [{
            'C': 'Q1x', 'P': 'hello', 'V': '1.2-r0', 'o': 'hello-origin'
        }]

    def test_signed_index_is_read(self):
        packages = apk.parse_index(make_index(self.records), self.keys)
        self.assertEqual([p['P'] for p in packages], ['hello'])

    def test_tampered_index_is_refused(self):
        index = make_index(self.records)
        streams = apk.split_streams(index)
        # Rebuild the second stream with one package's version changed, keep
        # the signature of the original.
        forged = _tar_gz({
            'DESCRIPTION': b'test',
            'APKINDEX': b'C:Q1x\nP:hello\nV:9.9-r0\n\n'
        })
        with self.assertRaisesRegex(apk.ApkError, 'BAD SIGNATURE'):
            apk.parse_index(streams[0][0] + forged, self.keys)

    def test_index_with_a_flipped_bit_is_refused(self):
        index = make_index(self.records)
        signature_length = len(apk.split_streams(index)[0][0])
        with self.assertRaises(apk.ApkError):
            apk.parse_index(flip(index, signature_length + 20), self.keys)

    def test_index_signed_by_an_unknown_key_is_refused(self):
        with self.assertRaisesRegex(apk.ApkError, 'not one of the'):
            apk.parse_index(make_index(self.records),
                            {'other-key.rsa.pub': KEY.pem})

    def test_truncated_index_is_refused(self):
        with self.assertRaises(apk.ApkError):
            apk.parse_index(make_index(self.records)[:-8], self.keys)


class ApkTest(unittest.TestCase):

    def setUp(self):
        self.keys = {KEY_FILE: KEY.pem}

    def test_signed_apk_verifies(self):
        data = apk.verify_apk(make_apk(), self.keys)
        self.assertTrue(data)

    def test_tampered_data_is_refused(self):
        streams = apk.split_streams(make_apk())
        # The last stream is the data tar: the signature covers only the
        # control stream, the datahash covers the data.
        forged = _tar_gz({'usr/share/hello': b'evil\n'})
        with self.assertRaisesRegex(apk.ApkError, 'datahash'):
            apk.verify_apk(streams[0][0] + streams[1][0] + forged, self.keys)

    def test_tampered_control_is_refused(self):
        package = make_apk()
        streams = apk.split_streams(package)
        control = _tar_gz({'.PKGINFO': b'pkgname = evil\ndatahash = 0\n'})
        with self.assertRaisesRegex(apk.ApkError, 'BAD SIGNATURE'):
            apk.verify_apk(streams[0][0] + control + streams[2][0], self.keys)

    def test_control_not_matching_the_index_is_refused(self):
        with self.assertRaisesRegex(apk.ApkError, 'does not match the index'):
            apk.verify_apk(make_apk(), self.keys, 'Q1AAAAAAAAAAAAAAAAAAAAAAAAAA=')

    def test_control_matching_the_index_is_accepted(self):
        package = make_apk()
        checksum = apk.control_checksum(apk.split_streams(package)[1][0])
        self.assertTrue(apk.verify_apk(package, self.keys, checksum))

    def test_unpack_refuses_a_tampered_apk_and_extracts_nothing(self):
        package = make_apk()
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, 'p.apk')
            with open(path, 'wb') as f:
                f.write(flip(package, len(package) - 40))
            out = os.path.join(tmp, 'root')
            with self.assertRaises(apk.ApkError):
                apk.unpack(path, self.keys, '', out)
            self.assertFalse(os.path.exists(out))

    def test_absolute_symlinks_become_relative(self):
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode='w') as archive:
            link = tarfile.TarInfo('usr/bin/tool')
            link.type = tarfile.SYMTYPE
            link.linkname = '/usr/lib/tool'
            archive.addfile(link)
        data = gzip.compress(buf.getvalue(), mtime=0)
        pkginfo = (f'datahash = {hashlib.sha256(data).hexdigest()}\n'
                   ).encode('ascii')
        control = _tar_gz({'.PKGINFO': pkginfo})
        package = _signature(control) + control + data
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, 'p.apk')
            with open(path, 'wb') as f:
                f.write(package)
            root = os.path.join(tmp, 'root')
            apk.unpack(path, self.keys, '', root)
            os.makedirs(os.path.join(root, 'usr/lib'))
            with open(os.path.join(root, 'usr/lib/tool'), 'w') as f:
                f.write('x')
            self.assertEqual(apk.finish(root), [])
            self.assertEqual(
                os.readlink(os.path.join(root, 'usr/bin/tool')),
                '../lib/tool')

    def test_dangling_symlinks_are_removed(self):
        with tempfile.TemporaryDirectory() as tmp:
            os.symlink('/usr/lib/nowhere', os.path.join(tmp, 'link'))
            self.assertEqual(apk.finish(tmp), ['link'])
            self.assertFalse(os.path.lexists(os.path.join(tmp, 'link')))


class RealPackageTest(unittest.TestCase):
    """A real Alpine package, signed by a key checked in under keys/."""

    def setUp(self):
        self.keys = apk.load_keys(KEY_FILES)
        with open(REAL_APK, 'rb') as f:
            self.package = f.read()

    def test_verifies(self):
        self.assertTrue(apk.verify_apk(self.package, self.keys))

    def test_tampered_copy_is_refused(self):
        streams = apk.split_streams(self.package)
        control_start = len(streams[0][0])
        with self.assertRaises(apk.ApkError):
            apk.verify_apk(flip(self.package, control_start + 20), self.keys)

    def test_unknown_key_is_refused(self):
        with self.assertRaisesRegex(apk.ApkError, 'not one of the'):
            apk.verify_apk(self.package, {'unrelated.rsa.pub': KEY.pem})

    def test_wrong_key_content_is_refused(self):
        name = 'alpine-devel@lists.alpinelinux.org-6165ee59.rsa.pub'
        with self.assertRaisesRegex(apk.ApkError, 'BAD SIGNATURE'):
            apk.verify_apk(self.package, {name: KEY.pem})


class ResolveTest(unittest.TestCase):

    PACKAGES = [
        {'P': 'app', 'V': '1.0-r0', 'o': 'app', 'D': 'so:libx.so.1 /bin/sh',
         'repo': 'main'},
        {'P': 'libx', 'V': '2.0-r1', 'o': 'x', 'p': 'so:libx.so.1=1',
         'repo': 'main'},
        {'P': 'libx', 'V': '2.0-r10', 'o': 'x', 'p': 'so:libx.so.1=1',
         'repo': 'main'},
        {'P': 'lonely', 'V': '3.1', 'repo': 'community'},
    ]

    def test_missing_package_names_the_package_and_the_branch(self):
        with self.assertRaises(apk.ApkError) as cm:
            apk.resolve(self.PACKAGES, ['app', 'nonesuch'], 'v3.24', False)
        self.assertIn("'nonesuch'", str(cm.exception))
        self.assertIn('Alpine v3.24', str(cm.exception))

    def test_takes_the_highest_version(self):
        got = apk.resolve(self.PACKAGES, ['libx'], 'v3.24', False)
        self.assertEqual([p['V'] for p in got], ['2.0-r10'])

    def test_closure_follows_providers(self):
        got = apk.resolve(self.PACKAGES, ['app'], 'v3.24', True)
        self.assertEqual([p['P'] for p in got], ['app', 'libx'])

    def test_closure_fails_on_an_unprovided_dependency(self):
        packages = self.PACKAGES + [{'P': 'bad', 'V': '1', 'D': 'so:nope.so'}]
        with self.assertRaisesRegex(apk.ApkError, 'so:nope.so'):
            apk.resolve(packages, ['bad'], 'v3.24', True)

    def test_command_line_reports_a_missing_package(self):
        with tempfile.TemporaryDirectory() as tmp:
            index = os.path.join(tmp, 'index.json')
            with open(index, 'w') as f:
                json.dump({'branch': 'v3.24', 'packages': self.PACKAGES}, f)
            stderr = io.StringIO()
            old = sys.stderr
            sys.stderr = stderr
            try:
                rc = apk.main(['resolve', '--index', index, '--name', 'zzz'])
            finally:
                sys.stderr = old
        self.assertEqual(rc, 1)
        self.assertIn("package 'zzz' is not in Alpine v3.24", stderr.getvalue())


if __name__ == '__main__':
    REAL_APK, *KEY_FILES = sys.argv[1:]
    unittest.main(argv=sys.argv[:1])
