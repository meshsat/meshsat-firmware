#!/usr/bin/env python3
"""Publish a meshsat-node-<version> tag's images to the GitHub release of the public mirror.

Run by the `release:github` CI job after the env build jobs. It reads their artifacts from
.pio/build/<env>/, writes SHA256SUMS and manifest.json, and creates (or refreshes) a normal
release on github.com/meshsat/meshsat-firmware with every file attached. The MeshSat apps read
https://github.com/meshsat/meshsat-firmware/releases/latest/download/manifest.json, so the
release is never a draft or a prerelease.

manifest.json (schema 1):
  {"schema": 1, "version": "<version>", "tag": "meshsat-node-<version>", "commit": "<sha>",
   "released_at": "<ISO UTC>",
   "images": [{"env": "<pio env>", "file": "<app image>", "size": <bytes>, "sha256": "<hex>"}]}
"images" lists the app image of each env (firmware-<env>-<version>.bin, what the BLE OTA
loader writes); the factory and littlefs images are release assets for USB flashing only.

Stdlib only. The GitHub token comes from GHCR_TOKEN and is never printed.
"""

import datetime
import glob
import hashlib
import json
import os
import sys
import time
import urllib.error
import urllib.request

REPO = os.environ.get("GITHUB_REPO", "meshsat/meshsat-firmware")
TAG_PREFIX = "meshsat-node-"
API = "https://api.github.com"
UPLOADS = "https://uploads.github.com"


def fail(msg):
    print(f"ERROR: {msg}", file=sys.stderr)
    sys.exit(1)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def gh(method, url, token, data=None, content_type="application/json", ok=(200, 201, 204)):
    """One GitHub API call. Returns (status, parsed JSON or None)."""
    body = None
    if data is not None:
        body = data if isinstance(data, bytes) else json.dumps(data).encode()
    req = urllib.request.Request(url, data=body, method=method)
    req.add_header("Authorization", f"token {token}")
    req.add_header("Accept", "application/vnd.github+json")
    req.add_header("X-GitHub-Api-Version", "2022-11-28")
    if body is not None:
        req.add_header("Content-Type", content_type)
    try:
        with urllib.request.urlopen(req, timeout=300) as resp:
            raw = resp.read()
            return resp.status, (json.loads(raw) if raw else None)
    except urllib.error.HTTPError as e:
        raw = e.read()
        if e.code in ok:
            return e.code, None
        try:
            return e.code, json.loads(raw)
        except ValueError:
            return e.code, {"message": raw[:300].decode(errors="replace")}


def collect(version):
    """The images of every env built in this pipeline, checked against the tag's version."""
    envs = sorted(os.path.basename(d) for d in glob.glob(".pio/build/*") if os.path.isdir(d))
    if not envs:
        fail("no .pio/build/<env> directories: did the build jobs' artifacts arrive?")
    assets, images = [], []
    for env in envs:
        app = f".pio/build/{env}/firmware-{env}-{version}.bin"
        if not os.path.isfile(app):
            found = sorted(os.path.basename(p) for p in glob.glob(f".pio/build/{env}/*.bin"))
            fail(f"{env}: {os.path.basename(app)} missing (built: {found}); the tag must name "
                 f"the version the build wrote, {TAG_PREFIX}<major.minor.build>.<sha7>")
        images.append({"env": env, "file": os.path.basename(app),
                       "size": os.path.getsize(app), "sha256": sha256(app)})
        assets.append(app)
        for extra in (f".pio/build/{env}/firmware-{env}-{version}.factory.bin",
                      f".pio/build/{env}/littlefs-{env}-{version}.bin"):
            if os.path.isfile(extra):
                assets.append(extra)
    return envs, assets, images


def main():
    tag = os.environ.get("CI_COMMIT_TAG", "")
    if not tag.startswith(TAG_PREFIX):
        fail(f"CI_COMMIT_TAG {tag!r} is not a {TAG_PREFIX}<version> tag")
    version = tag[len(TAG_PREFIX):]
    commit = os.environ.get("CI_COMMIT_SHA", "")
    if not version.endswith("." + commit[:7]):
        fail(f"tag version {version} does not end in the tagged commit's .{commit[:7]}")
    token = os.environ.get("GHCR_TOKEN", "")
    if not token:
        fail("GHCR_TOKEN is not set (it is a protected group variable: the tag must match "
             "the protected tag rule meshsat-node-*)")

    envs, assets, images = collect(version)
    out = "release-assets"
    os.makedirs(out, exist_ok=True)
    manifest = {
        "schema": 1,
        "version": version,
        "tag": tag,
        "commit": commit,
        "released_at": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "images": images,
    }
    with open(f"{out}/manifest.json", "w") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    files = assets + [f"{out}/manifest.json"]
    with open(f"{out}/SHA256SUMS", "w") as f:
        for p in sorted(files, key=os.path.basename):
            f.write(f"{sha256(p)}  {os.path.basename(p)}\n")
    files.append(f"{out}/SHA256SUMS")
    print(open(f"{out}/SHA256SUMS").read())
    print(json.dumps(manifest, indent=2))

    # The release must point at this commit. The push mirror may not have carried it yet.
    for attempt in range(31):
        st, _ = gh("GET", f"{API}/repos/{REPO}/commits/{commit}", token)
        if st == 200:
            break
        if attempt == 30:
            fail(f"commit {commit} is not on github.com/{REPO} after 15 min (push mirror?)")
        print(f"commit not on GitHub yet (HTTP {st}), waiting 30 s")
        time.sleep(30)

    rows = "\n".join(f"| `{i['env']}` | `{i['file']}` | `{i['sha256']}` |" for i in images)
    body = (
        f"MeshSat node firmware {version}, built from {commit[:12]} on this repository "
        f"(Meshtastic firmware plus the Iridium Bluetooth pipe in `src/meshsat/`).\n\n"
        f"| Env | App image (Bluetooth update) | SHA-256 |\n|---|---|---|\n{rows}\n\n"
        "MeshSat Android installs the app image over Bluetooth (Settings > Satellite > Node "
        "firmware) and checks its SHA-256 against `manifest.json`. The `.factory.bin` and "
        "`littlefs-*.bin` files are for flashing over USB, as the README describes. "
        "`SHA256SUMS` covers every file here.\n\n"
        "Prototype firmware: see the README's \"What is proven, and what is not\"."
    )
    meta = {"name": f"MeshSat node {version}", "body": body, "draft": False,
            "prerelease": False, "make_latest": "true"}

    st, rel = gh("GET", f"{API}/repos/{REPO}/releases/tags/{tag}", token)
    if st == 200 and rel:
        rid = rel["id"]
        st, rel = gh("PATCH", f"{API}/repos/{REPO}/releases/{rid}", token, meta)
        if st != 200:
            fail(f"release update HTTP {st}: {rel}")
        names = {os.path.basename(p) for p in files}
        for a in rel.get("assets", []):
            if a["name"] in names:
                gh("DELETE", f"{API}/repos/{REPO}/releases/assets/{a['id']}", token)
        print(f"updated release {rid}")
    else:
        st, rel = gh("POST", f"{API}/repos/{REPO}/releases", token,
                     dict(meta, tag_name=tag, target_commitish=commit))
        if st != 201:
            fail(f"release create HTTP {st}: {rel}")
        rid = rel["id"]
        print(f"created release {rid}")

    for p in files:
        name = os.path.basename(p)
        ctype = "application/json" if name.endswith(".json") else (
            "text/plain" if name == "SHA256SUMS" else "application/octet-stream")
        with open(p, "rb") as f:
            data = f.read()
        st, res = gh("POST", f"{UPLOADS}/repos/{REPO}/releases/{rid}/assets?name={name}",
                     token, data, content_type=ctype)
        if st != 201:
            fail(f"upload {name} HTTP {st}: {res}")
        print(f"uploaded {name} ({len(data)} bytes)")

    st, latest = gh("GET", f"{API}/repos/{REPO}/releases/latest", token)
    if st != 200 or latest.get("tag_name") != tag:
        fail(f"releases/latest is {latest.get('tag_name') if latest else st}, not {tag}")
    print(f"https://github.com/{REPO}/releases/tag/{tag}")
    print(f"latest manifest: https://github.com/{REPO}/releases/latest/download/manifest.json")


if __name__ == "__main__":
    main()
