"""Alpine APKINDEX and .apk verification, resolution and unpacking.

Plan step 24.1 (docs/plan/phases/24-alpine-series-pins.md). Run by
alpine.bzl's repository rules with the hermetic interpreter from
rules_python; standard library only (the RSA check is PKCS#1 v1.5
verification with Python's `pow`, which needs no key material beyond the
public key).

An .apk (apk v2) is three concatenated gzip streams: a tar holding the
signature (`.SIGN.RSA.<keyname>`, an RSA signature over the second stream's
bytes), a tar holding `.PKGINFO` (whose `datahash` is the sha256 of the
third stream), and the data tar. APKINDEX.tar.gz is two streams: the
signature, then a tar of DESCRIPTION and APKINDEX; the signature covers the
second stream. The index's `C:` field for a package is "Q1" plus the base64
of the SHA-1 of that package's control stream, so a verified index also
pins every package's control segment, and through `datahash` its data.

Subcommands (see main):
  index    verify the APKINDEX.tar.gz files and write them as one JSON file;
  resolve  pick the branch's current version of the named packages (and
           their dependency closure) from that JSON file;
  download download the resolved apks (a 404 means a stale index);
  unpack   verify one .apk and extract it;
  finish   fix the symlinks of the unpacked tree.
"""

import argparse
import base64
import concurrent.futures
import hashlib
import io
import json
import os
import re
import shutil
import sys
import tarfile
import urllib.error
import urllib.request
import zlib


class ApkError(Exception):
    """A verification or resolution failure, with a message for a person."""


def split_streams(data):
    """Splits concatenated gzip streams.

    Args:
        data: the bytes of the concatenation.

    Returns:
        A list of (raw, decompressed) pairs, one per stream; `raw` is the
        stream exactly as stored (what the signatures and hashes cover).

    Raises:
        ApkError: the data is not a sequence of gzip streams.
    """
    streams = []
    while data:
        decompressor = zlib.decompressobj(wbits=31)
        try:
            decompressed = decompressor.decompress(data)
        except zlib.error as e:
            raise ApkError(f'not a gzip stream: {e}') from e
        if not decompressor.eof:
            raise ApkError('truncated gzip stream')
        rest = decompressor.unused_data
        streams.append((data[:len(data) - len(rest)], decompressed))
        data = rest
    return streams


def _der(data, start=0):
    """Returns (tag, content, next_index) of the DER element at `start`."""
    tag = data[start]
    length = data[start + 1]
    pos = start + 2
    if length & 0x80:
        count = length & 0x7F
        length = int.from_bytes(data[pos:pos + count], 'big')
        pos += count
    return tag, data[pos:pos + length], pos + length


def _rsa_public_key(pem):
    """Returns (modulus, exponent) of a PEM SubjectPublicKeyInfo."""
    body = ''.join(
        line for line in pem.splitlines() if not line.startswith('-----'))
    der = base64.b64decode(body)
    _, spki, _ = _der(der)
    _, _, pos = _der(spki)  # the algorithm identifier
    _, bits, _ = _der(spki, pos)
    _, seq, _ = _der(bits[1:])  # skip the unused-bits byte
    _, modulus, pos = _der(seq)
    _, exponent, _ = _der(seq, pos)
    return int.from_bytes(modulus, 'big'), int.from_bytes(exponent, 'big')


# DER prefix of the DigestInfo structure RSASSA-PKCS1-v1_5 wraps a digest in.
_DIGEST_INFO = {
    'sha1': bytes.fromhex('3021300906052b0e03021a05000414'),
    'sha256': bytes.fromhex('3031300d060960864801650304020105000420'),
}


def rsa_verify(pem, algorithm, signed, signature):
    """Verifies an RSASSA-PKCS1-v1_5 signature.

    Args:
        pem: the PEM public key.
        algorithm: 'sha1' or 'sha256'.
        signed: the signed bytes.
        signature: the signature.

    Returns:
        True if the signature is valid.
    """
    modulus, exponent = _rsa_public_key(pem)
    size = (modulus.bit_length() + 7) // 8
    value = int.from_bytes(signature, 'big')
    if len(signature) != size or value >= modulus:
        return False
    padded = pow(value, exponent, modulus).to_bytes(size, 'big')
    digest = _DIGEST_INFO[algorithm] + hashlib.new(algorithm, signed).digest()
    expected = (b'\x00\x01' + b'\xff' * (size - len(digest) - 3) + b'\x00' +
                digest)
    return padded == expected


def verify_signature(signature_tar, signed, keys):
    """Checks a signature member against the checked-in keys.

    Args:
        signature_tar: the first stream, decompressed: a tar holding one
            `.SIGN.RSA[256].<keyname>` member.
        signed: the bytes the signature covers (the second stream, raw).
        keys: {key file name: PEM text} of the keys we trust.

    Returns:
        The name of the key that signed.

    Raises:
        ApkError: unknown key or bad signature.
    """
    try:
        archive = tarfile.open(fileobj=io.BytesIO(signature_tar))
        members = archive.getmembers()
    except tarfile.TarError as e:
        raise ApkError(f'unreadable signature segment: {e}') from e
    if len(members) != 1:
        raise ApkError(f'{len(members)} signature members, want 1')
    member = members[0]
    match = re.fullmatch(r'\.SIGN\.(RSA|RSA256)\.(.+)', member.name)
    if not match:
        raise ApkError(f'unknown signature member {member.name!r}')
    algorithm = 'sha1' if match.group(1) == 'RSA' else 'sha256'
    key_name = match.group(2)
    if key_name not in keys:
        raise ApkError(f'signed by key {key_name}, which is not one of the '
                       f'checked-in keys ({", ".join(sorted(keys))})')
    signature = archive.extractfile(member).read()
    if not rsa_verify(keys[key_name], algorithm, signed, signature):
        raise ApkError(f'BAD SIGNATURE ({member.name})')
    return key_name


def load_keys(paths):
    """Returns {key file name: PEM text} for the given key files."""
    keys = {}
    for path in paths:
        with open(path, encoding='ascii') as f:
            keys[os.path.basename(path)] = f.read()
    return keys


_BRANCH = re.compile(r'v[0-9]+\.[0-9]+')


def check_branch(branch):
    """Accepts only a release branch such as v3.24.

    Raises:
        ApkError: anything else (edge, latest-stable, a path).
    """
    if not _BRANCH.fullmatch(branch):
        raise ApkError(f'{branch!r} is not an Alpine release branch (vMAJOR.'
                       'MINOR, e.g. v3.24); edge and latest-stable move')


def parse_index(data, keys, branch=None):
    """Verifies an APKINDEX.tar.gz and returns its package records.

    Args:
        data: the bytes of APKINDEX.tar.gz.
        keys: as for verify_signature.
        branch: if given, the release branch the index must belong to: its
            DESCRIPTION member (aports' `git describe`, e.g.
            v3.24.2-90-gc2cd9709075) starts with the branch and a dot, so a
            mirror serving another branch's (validly signed) index at this
            branch's URL is caught.

    Returns:
        A list of dicts keyed by the index's one-letter fields (P name,
        V version, o origin, D dependencies, p provides, C control
        checksum, S size, k provider priority).

    Raises:
        ApkError: the index is malformed or its signature is bad.
    """
    streams = split_streams(data)
    if len(streams) != 2:
        raise ApkError(f'APKINDEX has {len(streams)} streams, want 2')
    verify_signature(streams[0][1], streams[1][0], keys)
    try:
        archive = tarfile.open(fileobj=io.BytesIO(streams[1][1]))
        text = archive.extractfile('APKINDEX').read().decode('utf-8')
        description = archive.extractfile('DESCRIPTION').read().decode(
            'utf-8').strip()
    except (tarfile.TarError, KeyError) as e:
        raise ApkError(f'unreadable APKINDEX: {e}') from e
    if branch is not None and not description.startswith(branch + '.'):
        raise ApkError(f'the index says it is {description!r}, not Alpine '
                       f'{branch}')
    packages = []
    for block in text.split('\n\n'):
        record = {}
        for line in block.split('\n'):
            if len(line) > 2 and line[1] == ':':
                record[line[0]] = line[2:]
        if 'P' in record:
            packages.append(record)
    return packages


_PRERELEASE_RANK = {'alpha': -4, 'beta': -3, 'pre': -2, 'rc': -1, 'p': 1}


def version_key(version):
    """Sort key approximating apk's version order (enough for releases)."""
    match = re.fullmatch(
        r'([0-9]+(?:\.[0-9]+)*)([a-z])?((?:_[a-z]+[0-9]*)*)(?:-r([0-9]+))?',
        version)
    if not match:
        raise ApkError(f'cannot parse version {version!r}')
    numbers = [int(n) for n in match.group(1).split('.')]
    suffixes = [(_PRERELEASE_RANK.get(name, 0), int(number or 0))
                for name, number in re.findall(r'_([a-z]+)([0-9]*)',
                                               match.group(3) or '')]
    return (numbers, match.group(2) or '', suffixes or [(0, 0)],
            int(match.group(4) or 0))


def _provides(packages):
    """Returns {name: [packages providing it]}, a package providing itself."""
    table = {}
    for package in packages:
        names = [package['P']]
        names += [p.partition('=')[0] for p in package.get('p', '').split()]
        for name in names:
            table.setdefault(name, []).append(package)
    return table


def _dependency_name(token):
    return re.split(r'[<>=~]', token, maxsplit=1)[0]


def _current(packages):
    """Returns {name: the package record with the highest version}."""
    current = {}
    for package in packages:
        name = package['P']
        if (name not in current or
                version_key(package['V']) > version_key(current[name]['V'])):
            current[name] = package
    return current


def closure(packages, roots):
    """Returns the roots and everything they depend on, roots first.

    A dependency (`D:`) is a package name, or a `so:`/`cmd:`/`pc:` provider
    name; conflicts (`!x`) and file dependencies (`/bin/sh`) are skipped.
    """
    by_name = _current(packages)
    providers = _provides(packages)
    seen, order, todo = set(), [], list(roots)
    while todo:
        package = todo.pop(0)
        if package['P'] in seen:
            continue
        seen.add(package['P'])
        order.append(package)
        for token in package.get('D', '').split():
            if token[0] in '!/':
                continue
            name = _dependency_name(token)
            if name in by_name:
                todo.append(by_name[name])
                continue
            candidates = providers.get(name)
            if not candidates:
                raise ApkError(f'{package["P"]} depends on {token}, which no '
                               'package of the index provides')
            todo.append(max(candidates, key=lambda c: int(c.get('k', '0'))))
    return order


def resolve(packages, names, branch, with_closure):
    """Picks the branch's current version of each named package.

    Args:
        packages: the merged index records (parse_index, plus a 'repo' key).
        names: the package names wanted.
        branch: the Alpine branch, for the error message.
        with_closure: also return the dependency closure.

    Returns:
        The selected package records, roots first.

    Raises:
        ApkError: a named package is not in the branch.
    """
    by_name = _current(packages)
    roots = []
    for name in names:
        if name not in by_name:
            raise ApkError(
                f'package {name!r} is not in Alpine {branch}; check the '
                'name (binary packages only: the subpackage is named '
                'e.g. linux-virt, not linux-lts) and the repositories the '
                'index covers')
        roots.append(by_name[name])
    return closure(packages, roots) if with_closure else roots


def apk_file_name(package):
    return f'{package["P"]}-{package["V"]}.apk'


def control_checksum(control_stream):
    """The index's C: field for a control stream."""
    return 'Q1' + base64.b64encode(hashlib.sha1(control_stream).digest()
                                   ).decode('ascii')


def verify_apk(data, keys, expected_checksum=''):
    """Verifies an .apk against the keys, the index and its own datahash.

    Args:
        data: the bytes of the .apk.
        keys: as for verify_signature.
        expected_checksum: the index's C: value for the package, if known.

    Returns:
        The decompressed data tar.

    Raises:
        ApkError: any check fails.
    """
    streams = split_streams(data)
    if len(streams) != 3:
        raise ApkError(f'{len(streams)} gzip streams, want 3')
    verify_signature(streams[0][1], streams[1][0], keys)
    checksum = control_checksum(streams[1][0])
    if expected_checksum and checksum != expected_checksum:
        raise ApkError(f'control checksum {checksum} does not match the '
                       f'index ({expected_checksum})')
    try:
        control = tarfile.open(fileobj=io.BytesIO(streams[1][1]))
        info = control.extractfile('.PKGINFO').read().decode('utf-8')
    except (tarfile.TarError, KeyError) as e:
        raise ApkError(f'unreadable .PKGINFO: {e}') from e
    datahash = re.search(r'^datahash = ([0-9a-f]{64})$', info, re.MULTILINE)
    if not datahash:
        raise ApkError('.PKGINFO has no datahash')
    if hashlib.sha256(streams[2][0]).hexdigest() != datahash.group(1):
        raise ApkError('data segment does not match the datahash')
    return streams[2][1]


def finish(root):
    """Makes the symlinks of an unpacked tree usable by Bazel, or fails.

    An absolute symlink target would point at the host, so it becomes
    relative to `root`; Bazel's glob() rejects dangling symlinks, so a link
    whose target is not in the tree (a package outside the closure
    provides it) is removed. A link that resolves, through any chain of
    links, to a path outside `root` would let host files into
    glob(["root/**"]): the unpacking filter already refuses such a link,
    this is the check that does not trust it.

    Args:
        root: the directory the apks were unpacked into.

    Returns:
        The paths (relative to root) of the removed dangling links.

    Raises:
        ApkError: a link resolves outside root.
    """
    root = os.path.abspath(root)
    links = []
    for directory, subdirs, files in os.walk(root):
        for name in subdirs + files:
            path = os.path.join(directory, name)
            if os.path.islink(path):
                links.append(path)
    for path in links:
        target = os.readlink(path)
        if target.startswith('/'):
            os.unlink(path)
            target = os.path.relpath(
                os.path.join(root, target.lstrip('/')), os.path.dirname(path))
            os.symlink(target, path)
    real_root = os.path.realpath(root)
    removed = []
    for path in links:
        resolved = os.path.realpath(path)
        if resolved != real_root and not resolved.startswith(real_root + os.sep):
            raise ApkError(f'{os.path.relpath(path, root)} is a symlink that '
                           f'resolves outside the package tree ({resolved})')
        if not os.path.exists(path):
            os.unlink(path)
            removed.append(os.path.relpath(path, root))
    return sorted(removed)


def _extraction_filter(member, dest):
    """tarfile filter: absolute symlinks become relative, then data_filter.

    data_filter refuses absolute names, names and links that leave `dest`,
    hard links outside it, and device nodes and other special files, and it
    drops setuid bits and owners. Alpine's packages use absolute symlinks
    (/usr/bin/x -> /bin/busybox), which data_filter would refuse, but which
    are harmless once relative to the tree.
    """
    if member.issym() and member.linkname.startswith('/'):
        member = member.replace(
            linkname=os.path.relpath(
                member.linkname,
                os.path.dirname('/' + member.name.lstrip('/'))),
            deep=False)
    return tarfile.data_filter(member, dest)


def unpack(apk_path, keys, expected_checksum, out_dir):
    """Verifies an .apk and extracts its files into out_dir.

    Raises:
        ApkError: a check failed, or the package holds an entry that would
            leave out_dir or a device node.
    """
    with open(apk_path, 'rb') as f:
        data_tar = verify_apk(f.read(), keys, expected_checksum)
    os.makedirs(out_dir, exist_ok=True)
    try:
        with tarfile.open(fileobj=io.BytesIO(data_tar)) as archive:
            archive.extractall(out_dir, filter=_extraction_filter)
    except tarfile.TarError as e:
        raise ApkError(f'{os.path.basename(apk_path)}: refusing to unpack: '
                       f'{e}') from e


_STALE_INDEX = ('the index snapshot names a build the mirror no longer '
                'serves; run `bazel fetch --force --repo=@alpine_index`')


def _download_one(url, path):
    """Downloads one file, with a reason a person can act on on failure."""
    try:
        with urllib.request.urlopen(url, timeout=300) as response:
            with open(path, 'wb') as f:
                shutil.copyfileobj(response, f)
    except urllib.error.HTTPError as e:
        if e.code == 404:
            raise ApkError(f'{os.path.basename(path)}: {_STALE_INDEX} '
                           f'({url})') from e
        raise ApkError(f'downloading {url}: HTTP {e.code}') from e
    except (urllib.error.URLError, OSError) as e:
        raise ApkError(f'downloading {url}: {e}') from e


def download(selected, mirror, arch, out_dir):
    """Downloads the selected apks in parallel into out_dir.

    Alpine's mirror drops a superseded build within about a week, so a 404
    means the index snapshot a repository was resolved from is stale.

    Args:
        selected: resolve's records ('file', 'repo', 'branch').
        mirror: the mirror's base URL.
        arch: the architecture directory.
        out_dir: where the apks go (created).

    Raises:
        ApkError: a download failed.
    """
    os.makedirs(out_dir, exist_ok=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        futures = [
            pool.submit(
                _download_one,
                f'{mirror}/{p["branch"]}/{p["repo"]}/{arch}/{p["file"]}',
                os.path.join(out_dir, p['file'])) for p in selected
        ]
        for future in futures:
            future.result()


def _cmd_download(args):
    with open(args.resolved, encoding='utf-8') as f:
        selected = json.load(f)
    download(selected, args.mirror.rstrip('/'), args.arch, args.out)


def _cmd_index(args):
    check_branch(args.branch)
    keys = load_keys(args.key)
    packages = []
    for spec in args.index:
        repo, _, path = spec.partition('=')
        with open(path, 'rb') as f:
            records = parse_index(f.read(), keys, args.branch)
        for record in records:
            record['repo'] = repo
            packages.append(record)
    with open(args.out, 'w', encoding='utf-8') as f:
        json.dump({'branch': args.branch, 'packages': packages}, f)
    print(f'{args.branch}: {len(packages)} packages in '
          f'{len(args.index)} repositories, signatures verified',
          file=sys.stderr)


def _cmd_resolve(args):
    with open(args.index, encoding='utf-8') as f:
        index = json.load(f)
    selected = resolve(index['packages'], args.name, index['branch'],
                       args.closure)
    json.dump([{
        'name': p['P'],
        'version': p['V'],
        'origin': p.get('o', p['P']),
        'repo': p['repo'],
        'file': apk_file_name(p),
        'checksum': p['C'],
        'branch': index['branch'],
    } for p in selected], sys.stdout)


def _cmd_finish(args):
    removed = finish(args.root)
    if removed:
        print(f'removed {len(removed)} dangling symlinks: '
              f'{", ".join(removed[:20])}', file=sys.stderr)


def _cmd_unpack(args):
    unpack(args.apk, load_keys(args.key), args.checksum, args.out)
    print(f'{os.path.basename(args.apk)}: verified and unpacked',
          file=sys.stderr)


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    subparsers = parser.add_subparsers(dest='command', required=True)

    index = subparsers.add_parser('index')
    index.add_argument('--key', action='append', required=True)
    index.add_argument('--branch', required=True)
    index.add_argument('--out', required=True)
    index.add_argument('index', nargs='+', help='repo=APKINDEX.tar.gz')
    index.set_defaults(handler=_cmd_index)

    resolver = subparsers.add_parser('resolve')
    resolver.add_argument('--index', required=True)
    resolver.add_argument('--name', action='append', required=True)
    resolver.add_argument('--closure', action='store_true')
    resolver.set_defaults(handler=_cmd_resolve)

    downloader = subparsers.add_parser('download')
    downloader.add_argument('--resolved', required=True,
                            help="resolve's output")
    downloader.add_argument('--mirror', required=True)
    downloader.add_argument('--arch', required=True)
    downloader.add_argument('--out', required=True)
    downloader.set_defaults(handler=_cmd_download)

    unpacker = subparsers.add_parser('unpack')
    unpacker.add_argument('--key', action='append', required=True)
    unpacker.add_argument('--apk', required=True)
    unpacker.add_argument('--checksum', default='')
    unpacker.add_argument('--out', required=True)
    unpacker.set_defaults(handler=_cmd_unpack)

    finisher = subparsers.add_parser('finish')
    finisher.add_argument('--root', required=True)
    finisher.set_defaults(handler=_cmd_finish)

    args = parser.parse_args(argv)
    try:
        args.handler(args)
    except ApkError as e:
        print(f'apk.py: {e}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
