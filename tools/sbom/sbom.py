"""SBOMs and OSV ignore checks for dcfs (plan step 5.3).

OSV-Scanner does not read MODULE.bazel.lock, so the pins are written out as
two CycloneDX SBOMs: shipped.cdx.json (what the dcfs binaries link; gates the
`osv` CI job) and testonly.cdx.json (everything else; informational).

The shipped set is the external repositories of the Bazel dependency graph
of //dcfs:main and //dcfs:main_static (//dcfs:linked_deps); each is a Bazel
module whose resolved version (MODULE.bazel.lock) and upstream git commit
are recorded in pins.json. OSV matches C/C++ projects by git commit only, and
the CLI takes commits only from git roots, so `git-roots` writes a detached
git root per shipped component (README.md). Test-only pins come from:

  * MODULE.bazel: every bazel_dep not shipped, and every http_archive /
    http_file / qemu_repo (plus QEMU's separately pinned dtc);
  * third_party/debian/debs.lock: the Debian packages of the NFS rootfs
    (pkg:deb/debian/<source package>@<version>, the form OSV's Debian
    ecosystem matches; the binary-to-source table is debian_sources.tsv);
  * .github/ci/prepare.sh: the pinned Bazelisk;
  * the Alpine repositories (third_party/alpine): each package a fetch took,
    from its resolved.json, as pkg:apk/alpine/<origin>@<version>?distro=
    alpine-<release> (the origin package, which is what OSV matches).

`pins.json` maps each pin to its purl; a pin without an entry is an error
(tools/sbom/sbom_test.py), so a new pin cannot slip past the scanner.
"""

import argparse
import ast
import datetime
import json
import os
import re
import subprocess
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


def parse_alpine_repos(text):
    """Returns {repository name: [package names]} of the alpine_package calls
    in MODULE.bazel's text (third_party/alpine)."""
    repos = {}
    for node in ast.parse(text).body:
        if not (isinstance(node, ast.Expr) and isinstance(node.value, ast.Call)
                and isinstance(node.value.func, ast.Name)
                and node.value.func.id == "alpine_package"):
            continue
        kwargs = {k.arg: _literal(k.value) for k in node.value.keywords if k.arg}
        repos[kwargs["name"]] = list(kwargs["packages"])
    return repos


def alpine_components(wanted, resolved):
    """Returns the test-only components of the fetched Alpine packages.

    Args:
        wanted: {repository name: [package names]} from MODULE.bazel.
        resolved: {repository name: text of its resolved.json}.

    Each package is reported under its origin package, which is the name
    OSV's Alpine advisories use (the binary linux-virt is linux-lts): a
    component named after a binary subpackage matches nothing, silently.
    """
    for name in sorted(wanted):
        if name not in resolved:
            raise SbomError(
                f"{name} is an alpine_package in MODULE.bazel but no"
                " resolved.json was given for it (--alpine"
                f" {name}=<its resolved.json>)")
    for name in sorted(resolved):
        if name not in wanted:
            raise SbomError(
                f"a resolved.json was given for {name}, which is not an"
                " alpine_package in MODULE.bazel")
    comps = []
    for name in sorted(wanted):
        doc = json.loads(resolved[name])
        got = {p["name"] for p in doc["packages"]}
        for package in wanted[name]:
            if package not in got:
                raise SbomError(
                    f"{name}: resolved.json does not list {package}")
        distro = "alpine-" + doc["branch"].lstrip("v")
        for p in doc["packages"]:
            comps.append({
                "type": "library",
                "name": p["origin"],
                "version": p["version"],
                "purl": (f"pkg:apk/alpine/{p['origin']}@{p['version']}"
                         f"?distro={distro}"),
                "properties": [
                    {"name": "dcfs:pin", "value": f"alpine:{p['name']}"},
                    {"name": "dcfs:kind", "value": "tool"},
                    {"name": "dcfs:osv-matchable", "value": "true"},
                    {"name": "dcfs:purl-type", "value": "apk"},
                ],
            })
    return comps


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


def parse_lock_versions(lock_text):
    """Returns {module: version} of the modules Bazel selected.

    MODULE.bazel.lock lists a MODULE.bazel file for every version considered
    but a source.json only for the selected one.
    """
    out = {}
    for url in json.loads(lock_text).get("registryFileHashes", {}):
        m = re.search(r"/modules/([^/]+)/([^/]+)/source\.json$", url)
        if m:
            if m.group(1) in out:
                raise SbomError(f"{m.group(1)}: two selected versions in MODULE.bazel.lock")
            out[m.group(1)] = m.group(2)
    return out


def graph_repos(graph_text):
    """Returns the external repositories (canonical names) of a label list."""
    repos = set()
    for line in graph_text.splitlines():
        m = re.match(r"@@([^/@]+)//", line.strip())
        if m:
            repos.add(m.group(1))
    return repos


def check_graph(graph_text, pins):
    """Every linked repository is shipped or build-only, and vice versa.

    Returns the linked shipped module names.
    """
    shipped = {m + "+": m for m in pins["shipped"]}
    build_only = set(pins["build_only"])
    repos = graph_repos(graph_text)
    if not repos:
        raise SbomError("the dependency graph (//dcfs:linked_deps) lists no external repository")
    for r in sorted(repos):
        if r not in shipped and r not in build_only:
            raise SbomError(
                f"{r} is linked into the dcfs binaries but has no entry under"
                " 'shipped' (or 'build_only') in tools/sbom/pins.json")
    for r in sorted(set(shipped) | build_only):
        if r not in repos:
            raise SbomError(
                f"{r.rstrip('+')} is listed in tools/sbom/pins.json but"
                " //dcfs:linked_deps does not link it: remove the entry")
    return sorted(shipped[r] for r in repos if r in shipped)


def shipped_component(module, pin, version, direct):
    if pin["version"] != version:
        raise SbomError(
            f"{module}: pins.json says version {pin['version']} but"
            f" MODULE.bazel.lock resolved {version}: update the tag and commit"
            " in tools/sbom/pins.json")
    if not re.fullmatch(r"[0-9a-f]{40}", pin["commit"]):
        raise SbomError(f"{module}: commit {pin['commit']!r} is not a 40-digit git hash")
    m = re.fullmatch(r"https://github\.com/([^/]+)/([^/]+)", pin["repo"])
    if not m:
        raise SbomError(f"{module}: repo {pin['repo']!r} is not a github.com URL")
    if pin["osv"] not in ("records", "no-records"):
        raise SbomError(f"{module}: osv must be 'records' or 'no-records'")
    return {
        "type": "library",
        "name": pin.get("name", module),
        "version": version,
        "purl": f"pkg:github/{m.group(1)}/{m.group(2)}@{pin['commit']}",
        "externalReferences": [{"type": "vcs", "url": pin["repo"]}],
        "properties": [
            {"name": "dcfs:pin", "value": ("bazel_dep:" if direct else "module:") + module},
            {"name": "dcfs:kind", "value": "code"},
            {"name": "dcfs:scope", "value": "shipped"},
            {"name": "dcfs:commit", "value": pin["commit"]},
            {"name": "dcfs:tag", "value": pin["tag"]},
            {"name": "dcfs:osv-matchable", "value": "true"},
            {"name": "dcfs:osv-records", "value": pin["osv"]},
            {"name": "dcfs:purl-type", "value": "github"},
        ],
    }


def write_git_roots(sbom, out_dir):
    """Writes one detached git root per component with a dcfs:commit.

    osv-scanner (scan source --include-git-root) reads HEAD of each git root
    it finds and asks OSV which advisories' git ranges contain that commit;
    no object database is needed. Returns the root directories.
    """
    roots = []
    for c in sbom["components"]:
        props = {p["name"]: p["value"] for p in c.get("properties", [])}
        commit = props.get("dcfs:commit")
        if not commit or not re.fullmatch(r"[0-9a-f]{40}", commit):
            raise SbomError(f"{c['name']}: no dcfs:commit to write a git root for")
        git = os.path.join(out_dir, c["name"], ".git")
        os.makedirs(os.path.join(git, "objects"), exist_ok=True)
        os.makedirs(os.path.join(git, "refs"), exist_ok=True)
        with open(os.path.join(git, "HEAD"), "w", encoding="utf-8") as f:
            f.write(commit + "\n")
        url = c["externalReferences"][0]["url"]
        with open(os.path.join(git, "config"), "w", encoding="utf-8") as f:
            f.write("[core]\n\trepositoryformatversion = 0\n\tbare = false\n"
                    f'[remote "origin"]\n\turl = {url}\n')
        roots.append(os.path.join(out_dir, c["name"]))
    return roots


def _git_ls_remote(repo, patterns):
    return subprocess.run(["git", "ls-remote", repo] + patterns, check=True,
                          capture_output=True, text=True).stdout


def verify_commits(shipped_pins, ls_remote=_git_ls_remote):
    """Checks every pinned (tag, commit) against the upstream repository.

    Needs the network, so it is not part of the Bazel test: the osv CI job
    runs it. Annotated tags are peeled (the ^{} line is the commit).
    """
    problems = []
    for module, pin in sorted(shipped_pins.items()):
        tag = pin["tag"]
        out = ls_remote(pin["repo"], [f"refs/tags/{tag}", f"refs/tags/{tag}^{{}}"])
        refs = {}
        for line in (out or "").splitlines():
            sha, _, ref = line.partition("\t")
            refs[ref] = sha
        got = refs.get(f"refs/tags/{tag}^{{}}") or refs.get(f"refs/tags/{tag}")
        if got is None:
            problems.append(f"{module}: tag {tag} not found in {pin['repo']}")
        elif got != pin["commit"]:
            problems.append(f"{module}: tag {tag} moved: upstream {got}, pins.json {pin['commit']}")
    return problems


def build(module_text, lock_text, graph_text, debs_lock, sources_tsv,
          prepare_sh, pins, alpine=None):
    """Returns {"shipped": CycloneDX doc, "testonly": CycloneDX doc}.

    `alpine` maps each alpine_package repository's name to the text of its
    resolved.json (what the fetch took from the Alpine branch).
    """
    deps, repos = parse_module(module_text)
    # Without the graph (the osv CI job has no Bazel build) every shipped
    # pin is used; //tools/sbom:sbom_test has already checked the pins
    # against the graph.
    linked = (check_graph(graph_text, pins) if graph_text is not None
              else sorted(pins["shipped"]))
    versions = parse_lock_versions(lock_text)
    shipped = []
    for module in linked:
        if module in pins["bazel_dep"]:
            raise SbomError(
                f"{module} is shipped: remove it from 'bazel_dep' in tools/sbom/pins.json")
        if module not in versions:
            raise SbomError(f"{module}: not selected in MODULE.bazel.lock")
        shipped.append(shipped_component(module, pins["shipped"][module],
                                         versions[module], module in deps))
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
        if name in pins["shipped"]:
            continue
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

    comps += alpine_components(parse_alpine_repos(module_text), alpine or {})

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
        "shipped": _doc("dcfs-shipped", shipped),
        "testonly": _doc("dcfs-testonly", unique),
    }


def _doc(name, components):
    return {
        "bomFormat": "CycloneDX",
        "specVersion": "1.5",
        "version": 1,
        "metadata": {"component": {"type": "application", "name": name}},
        "components": components,
    }


def pins_covered(sbom):
    """The pins an SBOM covers; takes one document or build()'s pair."""
    if "shipped" in sbom:
        return pins_covered(sbom["shipped"]) | pins_covered(sbom["testonly"])
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
    g = sub.add_parser("generate", help="write shipped.cdx.json and testonly.cdx.json")
    g.add_argument("--module", default="MODULE.bazel")
    g.add_argument("--lock", default="MODULE.bazel.lock")
    g.add_argument("--graph", default=None,
                   help="check the shipped pins against //dcfs:linked_deps"
                   " (bazel build //dcfs:linked_deps; bazel-bin/dcfs/linked_deps)")
    g.add_argument("--debs-lock", default="third_party/debian/debs.lock")
    g.add_argument("--debian-sources", default="tools/sbom/debian_sources.tsv")
    g.add_argument("--prepare-sh", default=".github/ci/prepare.sh")
    g.add_argument("--pins", default="tools/sbom/pins.json")
    g.add_argument("--alpine", action="append", default=[],
                   metavar="NAME=PATH",
                   help="resolved.json of an alpine_package repository (in"
                   " Bazel's output base after a fetch: external/+alpine_"
                   "package+NAME/resolved.json), once per repository")
    g.add_argument("--out-dir", required=True)
    r = sub.add_parser("git-roots", help="write a detached git root per SBOM component")
    r.add_argument("--sbom", required=True)
    r.add_argument("--out-dir", required=True)
    ar = sub.add_parser("alpine-repos",
                        help="print the alpine_package repositories of MODULE.bazel")
    ar.add_argument("--module", default="MODULE.bazel")
    v = sub.add_parser("verify-commits", help="check pinned tags against upstream (network)")
    v.add_argument("--pins", default="tools/sbom/pins.json")
    c = sub.add_parser("check-ignores")
    c.add_argument("--config", default="osv-scanner.toml")
    c.add_argument("--today", default=None, help="YYYY-MM-DD (default: today, UTC)")
    a = ap.parse_args(argv)
    read = lambda p: open(p, encoding="utf-8").read()
    try:
        if a.cmd == "generate":
            alpine = {}
            for spec in a.alpine:
                name, _, path = spec.partition("=")
                alpine[name] = read(path)
            docs = build(read(a.module), read(a.lock),
                         read(a.graph) if a.graph else None, read(a.debs_lock),
                         read(a.debian_sources), read(a.prepare_sh),
                         json.loads(read(a.pins)), alpine=alpine)
            os.makedirs(a.out_dir, exist_ok=True)
            for kind in ("shipped", "testonly"):
                path = os.path.join(a.out_dir, kind + ".cdx.json")
                with open(path, "w", encoding="utf-8") as f:
                    json.dump(docs[kind], f, indent=2, sort_keys=True)
                    f.write("\n")
                print(f"SBOM {path}: {len(docs[kind]['components'])} entries")
        elif a.cmd == "alpine-repos":
            for name in sorted(parse_alpine_repos(read(a.module))):
                print(name)
        elif a.cmd == "git-roots":
            roots = write_git_roots(json.loads(read(a.sbom)), a.out_dir)
            print(f"{len(roots)} git roots under {a.out_dir}")
        elif a.cmd == "verify-commits":
            problems = verify_commits(json.loads(read(a.pins))["shipped"])
            for p in problems:
                print(f"pins.json: {p}", file=sys.stderr)
            return 1 if problems else 0
        else:
            today = (datetime.date.fromisoformat(a.today) if a.today
                     else datetime.datetime.now(datetime.timezone.utc).date())
            problems = check_ignores(read(a.config), today)
            for p in problems:
                print(f"osv-scanner.toml: {p}", file=sys.stderr)
            return 1 if problems else 0
    except SbomError as e:
        print(f"sbom: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
