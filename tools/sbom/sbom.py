"""SBOM and OSV ignore checks for dcfs (plan step 5.3).

OSV-Scanner does not read MODULE.bazel.lock, so the pins are written out as a
CycloneDX SBOM. Pins come from:

  * MODULE.bazel: every bazel_dep, and every http_archive / http_file /
    qemu_repo (plus QEMU's separately pinned dtc);
  * third_party/debian/debs.lock: the Debian packages of the NFS rootfs
    (pkg:deb/debian/<source package>@<version>, the form OSV's Debian
    ecosystem matches; the binary-to-source table is debian_sources.tsv);
  * .github/ci/prepare.sh: the pinned Bazelisk.

`pins.json` maps each pin to its purl; a pin without an entry is an error
(tools/sbom/sbom_test.py), so a new pin cannot slip past the scanner.
"""

import argparse
import ast
import datetime
import json
import re
import sys
import tomllib


class SbomError(Exception):
    pass


def _literal(node):
    try:
        return ast.literal_eval(node)
    except ValueError:
        return None


def parse_module(text):
    """Returns (bazel_deps, repositories) from MODULE.bazel's text.

    bazel_deps: {name: version}. repositories: {name: kwargs} for the
    top-level http_archive / http_file / qemu_repo calls.
    """
    deps, repos = {}, {}
    for node in ast.parse(text).body:
        if not (isinstance(node, ast.Expr) and isinstance(node.value, ast.Call)):
            continue
        call = node.value
        if not isinstance(call.func, ast.Name):
            continue
        kwargs = {k.arg: _literal(k.value) for k in call.keywords if k.arg}
        fn = call.func.id
        if fn == "bazel_dep":
            deps[kwargs["name"]] = kwargs["version"]
        elif fn in ("http_archive", "http_file", "qemu_repo"):
            repos[kwargs["name"]] = kwargs
    return deps, repos


def _repo_urls(kwargs):
    urls = kwargs.get("urls") or [kwargs.get("url")]
    return [u for u in urls if u]


def repo_version(name, kwargs, how):
    if how == "strip_prefix" or how == "dtc_strip_prefix":
        prefix = kwargs.get(how)
        if not prefix:
            raise SbomError(f"{name}: no {how} to take a version from")
        m = re.match(r".*?-(?:v)?([0-9][^/]*|[0-9a-f]{40}|r[0-9]+)$", prefix)
        if not m:
            raise SbomError(f"{name}: no version in {how} {prefix!r}")
        return m.group(1)
    if how.startswith("url:"):
        for url in _repo_urls(kwargs):
            m = re.search(how[4:], url)
            if m:
                return m.group(1)
        raise SbomError(f"{name}: {how} matches none of {_repo_urls(kwargs)}")
    raise SbomError(f"{name}: unknown version rule {how!r}")


def parse_debs(debs_lock, sources_tsv):
    """Returns [(binary, source, version, source_version)] for debs.lock."""
    table = {}
    for line in sources_tsv.splitlines():
        if not line or line.startswith("#"):
            continue
        binary, source, version, source_version = line.split("\t")
        table[(binary, version.split(":", 1)[-1])] = (source, version, source_version)
    out = []
    for line in debs_lock.splitlines():
        if not line.strip():
            continue
        filename = line.split()[0]
        binary, version, _arch = filename[: -len(".deb")].split("_")
        version = version.replace("%3a", "")
        if (binary, version) not in table:
            raise SbomError(
                f"{filename}: no row in debian_sources.tsv (binary package,"
                " source package, versions); see tools/sbom/README.md")
        source, full, source_version = table[(binary, version)]
        out.append((binary, source, full, source_version))
    return out


def parse_bazelisk(prepare_sh):
    m = re.search(r"^BAZELISK_VERSION=v?(\S+)$", prepare_sh, re.M)
    if not m:
        raise SbomError("BAZELISK_VERSION not found in .github/ci/prepare.sh")
    return m.group(1)


def build(module_text, debs_lock, sources_tsv, prepare_sh, pins):
    """Returns the CycloneDX document (a dict)."""
    deps, repos = parse_module(module_text)
    comps = []

    def add(name, version, purl, pin, kind, osv):
        comps.append({
            "type": "library",
            "name": name,
            "version": version,
            "purl": f"{purl}@{version}",
            "properties": [
                {"name": "dcfs:pin", "value": pin},
                {"name": "dcfs:kind", "value": kind},
                {"name": "dcfs:osv-matchable", "value": "true" if osv else "false"},
                {"name": "dcfs:purl-type", "value": purl.split("/")[0][4:]},
            ],
        })

    for name, version in sorted(deps.items()):
        pin = pins["bazel_dep"].get(name)
        if pin is None:
            raise SbomError(f"bazel_dep {name}: no entry in tools/sbom/pins.json")
        add(name, version, pin["purl"], f"bazel_dep:{name}", pin["kind"], False)

    for name, kwargs in sorted(repos.items()):
        pin = pins["repository"].get(name)
        if pin is None:
            raise SbomError(f"repository {name}: no entry in tools/sbom/pins.json")
        version = repo_version(name, kwargs, pin["version"])
        add(pin["purl"].split("/")[-1], version, pin["purl"], f"repository:{name}",
            pin["kind"], False)
        if kwargs.get("dtc_url"):
            dtc = pins["repository"].get(name + ".dtc")
            if dtc is None:
                raise SbomError(f"repository {name}.dtc: no entry in pins.json")
            add("dtc", repo_version(name + ".dtc", kwargs, dtc["version"]),
                dtc["purl"], f"repository:{name}.dtc", dtc["kind"], False)

    if "bazelisk" not in pins["script"]:
        raise SbomError("bazelisk: no entry in tools/sbom/pins.json")
    add("bazelisk", parse_bazelisk(prepare_sh), pins["script"]["bazelisk"]["purl"],
        "script:bazelisk", "tool", False)

    for binary, source, full, source_version in parse_debs(debs_lock, sources_tsv):
        # OSV's Debian advisories are per source package, so the entry is
        # the source package and its version (the tsv column already carries
        # the source's own epoch, or the binary's when Source: names none).
        sv = source_version
        comps.append({
            "type": "library",
            "name": source,
            "version": sv,
            "purl": f"pkg:deb/debian/{source}@{sv.replace(chr(58), '%3A')}?distro=bookworm",
            "properties": [
                {"name": "dcfs:pin", "value": f"deb:{binary}"},
                {"name": "dcfs:kind", "value": "code"},
                {"name": "dcfs:osv-matchable", "value": "true"},
                {"name": "dcfs:purl-type", "value": "deb"},
            ],
        })

    # One entry per distinct (purl): binary packages of one source collapse.
    seen, unique = set(), []
    for c in comps:
        key = c["purl"]
        if key in seen:
            for u in unique:
                if u["purl"] == key:
                    u["properties"].append({"name": "dcfs:pin", "value": c["properties"][0]["value"]})
            continue
        seen.add(key)
        unique.append(c)
    return {
        "bomFormat": "CycloneDX",
        "specVersion": "1.5",
        "version": 1,
        "metadata": {"component": {"type": "application", "name": "dcfs-pins"}},
        "components": unique,
    }


def pins_covered(sbom):
    return {p["value"] for c in sbom["components"] for p in c["properties"]
            if p["name"] == "dcfs:pin"}


def check_ignores(toml_text, today):
    """Returns a list of problems with osv-scanner.toml's ignores."""
    problems = []
    cfg = tomllib.loads(toml_text)
    for entry in cfg.get("IgnoredVulns", []):
        ident = entry.get("id", "<no id>")
        if not str(entry.get("reason", "")).strip():
            problems.append(f"{ident}: ignore has no reason")
        until = entry.get("ignoreUntil")
        if until is None:
            problems.append(f"{ident}: ignore has no ignoreUntil (expiry) date")
            continue
        if isinstance(until, datetime.datetime):
            until = until.date()
        if not isinstance(until, datetime.date):
            problems.append(f"{ident}: ignoreUntil is not a date: {until!r}")
        elif until < today:
            problems.append(f"{ident}: ignore expired on {until}")
    return problems


def main(argv):
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("generate")
    g.add_argument("--module", default="MODULE.bazel")
    g.add_argument("--debs-lock", default="third_party/debian/debs.lock")
    g.add_argument("--debian-sources", default="tools/sbom/debian_sources.tsv")
    g.add_argument("--prepare-sh", default=".github/ci/prepare.sh")
    g.add_argument("--pins", default="tools/sbom/pins.json")
    g.add_argument("--out", required=True)
    c = sub.add_parser("check-ignores")
    c.add_argument("--config", default="osv-scanner.toml")
    c.add_argument("--today", default=None, help="YYYY-MM-DD (default: today, UTC)")
    a = ap.parse_args(argv)
    try:
        if a.cmd == "generate":
            read = lambda p: open(p, encoding="utf-8").read()
            doc = build(read(a.module), read(a.debs_lock), read(a.debian_sources),
                        read(a.prepare_sh), json.loads(read(a.pins)))
            with open(a.out, "w", encoding="utf-8") as f:
                json.dump(doc, f, indent=2, sort_keys=True)
                f.write("\n")
            deb = sum(c["purl"].startswith("pkg:deb/") for c in doc["components"])
            print(f"SBOM {a.out}: {len(doc['components'])} entries, {deb} matchable by OSV")
        else:
            today = (datetime.date.fromisoformat(a.today) if a.today
                     else datetime.datetime.now(datetime.timezone.utc).date())
            problems = check_ignores(open(a.config, encoding="utf-8").read(), today)
            for p in problems:
                print(f"osv-scanner.toml: {p}", file=sys.stderr)
            return 1 if problems else 0
    except SbomError as e:
        print(f"sbom: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
