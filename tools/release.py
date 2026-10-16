"""Make a release: build from this checkout, package, tag, publish.

    py -3.13 tools/release.py 0.2.0             # the whole release
    py -3.13 tools/release.py 0.2.0 --dry-run   # build and package, change nothing
    py -3.13 tools/release.py 0.2.0 --upload-only   # attach the zip to an existing tag's release

See RELEASING.md for the procedure around it. In order, this:

  1. checks the tree is clean, on main, that CHANGELOG.md has a "## 0.2.0"
     section, and that the tag v0.2.0 does not exist yet;
  2. builds from scratch-free state: tools/fetch_ndsrecomp.sh if needed, then
     port/build.sh, which ends by running the unit suite (a red suite stops
     the release);
  3. packages dist/LSW2DS-PC-v0.2.0.zip with tools/package_build.py, whose
     README names this commit and the source URL (the GPL's source offer);
  4. tags v0.2.0 (annotated, with the changelog section as its message) and
     pushes main and the tag - which starts .github/workflows/release.yml;
  5. finds or creates the GitHub release for the tag, sets its notes from the
     changelog, and uploads the zip and a SHA256SUMS.txt beside it.

THE BINARY IS BUILT HERE, NOT IN CI, because building lifts the game's code
from a ROM and CI has no ROM - nor may it. What CI can check without one, it
checks (.github/workflows/ci.yml).

THE TOKEN. Step 5 needs a GitHub token with "contents: write" on this
repository: GITHUB_TOKEN in the environment, or else whatever `git credential
fill` returns for github.com (the same credential `git push` uses).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REPO = os.environ.get("LSW2_RELEASE_REPO", "AgitationSkeleton/lsw2ds-pc")
API = "https://api.github.com"
UPLOADS = "https://uploads.github.com"


def sh(*args: str, check: bool = True, capture: bool = False) -> str:
    print("$ " + " ".join(args), flush=True)
    r = subprocess.run(list(args), cwd=ROOT, text=True,
                       capture_output=capture)
    if check and r.returncode != 0:
        if capture:
            sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit(f"failed ({r.returncode}): {' '.join(args)}")
    return (r.stdout or "").strip() if capture else ""


def changelog_section(version: str) -> str:
    text = (ROOT / "CHANGELOG.md").read_text(encoding="utf-8")
    m = re.search(rf"^## {re.escape(version)}\b.*?$(.*?)(?=^## |\Z)", text,
                  re.M | re.S)
    if not m:
        raise SystemExit(f'CHANGELOG.md has no "## {version}" section - '
                         "move the Unreleased entries under it first")
    body = m.group(1).strip()
    if not body:
        raise SystemExit(f"CHANGELOG.md's {version} section is empty")
    return body


def token() -> str:
    if os.environ.get("GITHUB_TOKEN"):
        return os.environ["GITHUB_TOKEN"]
    r = subprocess.run(["git", "credential", "fill"], input="protocol=https\nhost=github.com\n\n",
                       text=True, capture_output=True)
    for line in r.stdout.splitlines():
        if line.startswith("password="):
            return line[len("password="):]
    raise SystemExit("no GitHub token: set GITHUB_TOKEN or sign git in to github.com")


def api(method: str, url: str, tok: str, body=None, data: bytes | None = None,
        content_type: str = "application/json"):
    if body is not None:
        data = json.dumps(body).encode()
    req = urllib.request.Request(url, data=data, method=method)
    req.add_header("Authorization", f"Bearer {tok}")
    req.add_header("Accept", "application/vnd.github+json")
    req.add_header("X-GitHub-Api-Version", "2022-11-28")
    if data is not None:
        req.add_header("Content-Type", content_type)
    try:
        with urllib.request.urlopen(req, timeout=600) as r:
            raw = r.read()
            return r.status, (json.loads(raw) if raw else {})
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"{}")


def publish(version: str, tag: str, notes: str, files: list[Path]) -> str:
    tok = token()
    st, rel = api("GET", f"{API}/repos/{REPO}/releases/tags/{tag}", tok)
    if st == 404:
        st, rel = api("POST", f"{API}/repos/{REPO}/releases", tok,
                      {"tag_name": tag, "name": f"v{version}", "body": notes,
                       "prerelease": version.startswith("0.")})
        if st not in (200, 201):
            raise SystemExit(f"could not create the release: {st} {rel}")
    elif st == 200:
        api("PATCH", f"{API}/repos/{REPO}/releases/{rel['id']}", tok, {"body": notes})
    else:
        raise SystemExit(f"could not look up the release: {st} {rel}")
    existing = {a["name"]: a["id"] for a in rel.get("assets", [])}
    for f in files:
        if f.name in existing:
            api("DELETE", f"{API}/repos/{REPO}/releases/assets/{existing[f.name]}", tok)
        print(f"uploading {f.name} ({f.stat().st_size / 1e6:.1f} MB)", flush=True)
        st, asset = api("POST",
                        f"{UPLOADS}/repos/{REPO}/releases/{rel['id']}/assets?name={f.name}",
                        tok, data=f.read_bytes(),
                        content_type="application/zip" if f.suffix == ".zip" else "text/plain")
        if st not in (200, 201):
            raise SystemExit(f"upload of {f.name} failed: {st} {asset}")
    return rel["html_url"]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("version", help="e.g. 0.2.0 (the tag is v0.2.0)")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--upload-only", action="store_true",
                    help="skip building and tagging; upload dist/ for an existing tag")
    args = ap.parse_args()
    version = args.version.lstrip("v")
    if not re.fullmatch(r"\d+\.\d+\.\d+(-[0-9A-Za-z.]+)?", version):
        raise SystemExit(f"{version!r} is not a version like 0.2.0")
    tag = f"v{version}"
    notes = changelog_section(version)
    archive = ROOT / "dist" / f"LSW2DS-PC-{tag}.zip"
    sums = ROOT / "dist" / "SHA256SUMS.txt"

    if not args.upload_only:
        if sh("git", "status", "--porcelain", capture=True):
            raise SystemExit("the working tree has uncommitted changes")
        branch = sh("git", "rev-parse", "--abbrev-ref", "HEAD", capture=True)
        if branch != "main" and not args.dry_run:
            raise SystemExit(f"releases are made from main, not {branch}")
        if sh("git", "tag", "--list", tag, capture=True):
            raise SystemExit(f"{tag} already exists")

        sh("bash", "tools/fetch_ndsrecomp.sh")
        sh("bash", "port/build.sh")
        sh(sys.executable, "tools/package_build.py", "--version", tag)
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        sums.write_text(f"{digest}  {archive.name}\n", encoding="utf-8")

        if args.dry_run:
            print(f"\ndry run: {archive} built; nothing tagged or pushed")
            return 0
        sh("git", "tag", "-a", tag, "-m", f"v{version}\n\n{notes}")
        sh("git", "push", "origin", "main", tag)

    if not archive.is_file():
        raise SystemExit(f"{archive} is missing - run without --upload-only")
    url = publish(version, tag, notes, [archive, sums])
    print(f"\nreleased {tag}: {url}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
