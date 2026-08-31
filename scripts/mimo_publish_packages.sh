#!/usr/bin/env bash
# Build the public package registry under site/packages/ from registry/packages/.
#
#   scripts/mimo_publish_packages.sh
#
# Layout:
#   site/packages/index.json
#   site/packages/<name>/<version>/<name>.tar.gz
#   site/packages/index.html   (browse page)

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/registry/packages"
OUT="$ROOT/site/packages"
ORIGIN="${MIMO_PKG_ORIGIN:-https://lukelang.org/packages}"
FALLBACK_ORIGIN="https://packages.lukelang.org"

rm -rf "$OUT"
mkdir -p "$OUT"

python3 - "$SRC" "$OUT" "$ORIGIN" "$FALLBACK_ORIGIN" <<'PY'
import hashlib, json, os, sys, tarfile, datetime
from pathlib import Path

src, out = Path(sys.argv[1]), Path(sys.argv[2])
origin, fallback = sys.argv[3], sys.argv[4]
packages = {}

builtins = {
    "http":     {"import": "std/http",     "description": "HTTP client helpers — ships with LukeLang"},
    "json":     {"import": "std/json",     "description": "JSON encode/decode — ships with LukeLang"},
    "server":   {"import": "std/server",   "description": "HTTP server — ships with LukeLang"},
    "postgres": {"import": "std/pg",       "description": "Postgres via libpq — ships with LukeLang", "aliases": ["pg"]},
    "pg":       {"import": "std/pg",       "description": "Postgres via libpq — ships with LukeLang"},
    "sqlite":   {"import": "std/sqlite",   "description": "SQLite — ships with LukeLang"},
    "env":      {"import": "std/env",      "description": "Process environment — ships with LukeLang"},
    "auth":     {"import": "std/auth",     "description": "Argon2id passwords + sessions — ships with LukeLang"},
    "jwt":      {"import": "std/auth",     "description": "Auth surface (sessions/CSRF today; JWT helpers expanding)", "aliases": []},
    "crypto":   {"import": "std/auth",     "description": "Crypto primitives via std/auth (Argon2id) — ships with LukeLang"},
    "args":     {"import": "std/args",     "description": "CLI args — ships with LukeLang"},
    "files":    {"import": "std/files",    "description": "Filesystem helpers — ships with LukeLang"},
    "logger":   {"import": None, "description": "Planned first-party logger — not published yet", "status": "planned"},
    "mysql":    {"import": None, "description": "Planned MySQL driver — not published yet", "status": "planned"},
    "redis":    {"import": None, "description": "Planned Redis client — not published yet", "status": "planned"},
}

for name, meta in builtins.items():
    entry = {
        "version": "0.3.0",
        "kind": "builtin" if meta.get("import") else "planned",
        "description": meta["description"],
    }
    if meta.get("import"):
        entry["import"] = meta["import"]
    if meta.get("status"):
        entry["status"] = meta["status"]
    packages[name] = entry

for pkg_dir in sorted(p for p in src.iterdir() if p.is_dir()):
    name = pkg_dir.name
    pkg_meta = {}
    luke_pkg = pkg_dir / "luke.pkg"
    if luke_pkg.is_file():
        for line in luke_pkg.read_text(encoding="utf-8").splitlines():
            if "=" in line:
                k, v = line.split("=", 1)
                pkg_meta[k.strip()] = v.strip()
    version = pkg_meta.get("version", "0.1.0")
    desc = pkg_meta.get("description", f"luke/{name}")
    verd = out / name / version
    verd.mkdir(parents=True, exist_ok=True)
    tarball = verd / f"{name}.tar.gz"
    with tarfile.open(tarball, "w:gz") as tar:
        for f in sorted(pkg_dir.iterdir()):
            if f.is_file():
                tar.add(f, arcname=f"{name}/{f.name}")
    sha = hashlib.sha256(tarball.read_bytes()).hexdigest()
    # Integrity field luke PKG expects: sha of luke.pkg+main.luke contents
    blob = b""
    for part in ("luke.pkg", "main.luke", "main.lk"):
        p = pkg_dir / part
        if p.is_file():
            blob += p.read_bytes()
    content_sha = hashlib.sha256(blob).hexdigest() if blob else sha
    url = f"{origin}/{name}/{version}/{name}.tar.gz"
    packages[name] = {
        "version": version,
        "kind": "package",
        "description": desc,
        "url": url,
        "url_fallback": f"{fallback}/{name}/{version}/{name}.tar.gz",
        "sha256": content_sha,
        "archive_sha256": sha,
        "tarball": f"{name}/{version}/{name}.tar.gz",
    }
    print(f"  + {name}@{version} ({sha[:12]}…)")

doc = {
    "name": "lukelang-packages",
    "version": "0.3.0",
    "description": "Official LukeLang package index — mimo add <name>",
    "home": str(origin),
    "updated": datetime.date.today().isoformat(),
    "packages": packages,
}
(out / "index.json").write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")

# Browse page
rows = []
for name, meta in sorted(packages.items()):
    kind = meta.get("kind", "package")
    ver = meta.get("version", "")
    desc = meta.get("description", "")
    if kind == "package":
        link = f'<a class="btn btn--dl" href="{meta["tarball"]}">Download</a>'
        badge = "package"
    elif kind == "builtin":
        link = f'<code>import {meta["import"]}</code>'
        badge = "builtin"
    else:
        link = "<span>coming soon</span>"
        badge = "planned"
    rows.append(
        f'<li><p class="rows__key">{name} <i>{badge}</i></p>'
        f'<p class="rows__val">{desc}<br/>{link}</p></li>'
    )

html = f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8" />
<meta name="viewport" content="width=device-width, initial-scale=1" />
<title>Packages — LukeLang</title>
<meta name="description" content="Official LukeLang package registry for mimo add." />
<link rel="icon" href="https://lukelang.org/assets/favicon.png" type="image/png" />
<link rel="stylesheet" href="https://lukelang.org/styles.css" />
<link rel="stylesheet" href="https://lukelang.org/pages.css" />
<link href="https://fonts.googleapis.com/css2?family=Plus+Jakarta+Sans:ital,wght@0,200..800;1,200..800&display=swap" rel="stylesheet" />
</head>
<body>
<div class="grain" aria-hidden="true"></div>
<header class="nav">
  <a class="nav__brand" href="https://lukelang.org/">
    <img src="https://lukelang.org/assets/luke-mark-sm.png" alt="" width="128" height="65" />
    <span>LukeLang</span>
  </a>
  <nav class="nav__links" aria-label="Primary">
    <a href="https://lukelang.org/learn/">Learn</a>
    <a href="https://lukelang.org/docs/">Docs</a>
    <a href="https://lukelang.org/download/">Download</a>
    <a href="./" aria-current="page">Packages</a>
    <a href="https://github.com/lucasdmarshall/LukeLang">GitHub</a>
  </nav>
</header>
<main>
  <section class="masthead">
    <p class="masthead__kicker">lukelang.org/packages</p>
    <h1>mimo <em>add</em></h1>
    <p>Official package index. Builtin modules ship with the compiler; packages install into <code>luke_modules/</code>.</p>
  </section>
  <div class="page">
    <div class="prose" style="grid-column: 1 / -1; max-width: 52rem; margin: 0 auto;">
      <section>
        <h2>Install</h2>
<pre><code><span class="ln"><b class="sh">mimo</b> add greeter</span>
<span class="ln"><b class="sh">mimo</b> add http          <b class="cm"># builtin → import std/http</b></span></code></pre>
        <p>Machine index: <a href="./index.json"><code>index.json</code></a></p>
      </section>
      <section>
        <h2>Catalog</h2>
        <ul class="rows rows--dl">
          {''.join(rows)}
        </ul>
      </section>
    </div>
  </div>
</main>
</body>
</html>
"""
(out / "index.html").write_text(html, encoding="utf-8")
print(f"wrote {out}/index.json with {len(packages)} entries")
PY

echo "mimo packages ready under site/packages/"
