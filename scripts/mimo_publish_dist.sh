#!/usr/bin/env bash
# Assemble site/dist/mimo from packaged artifacts and sync bootstrap scripts.
#
#   # after building one or more targets into dist/mimo-out/
#   scripts/mimo_publish_dist.sh
#
# Layout:
#   site/mimo                          bootstrap (curl | bash)
#   site/mimo.ps1                      bootstrap (irm | iex)
#   site/dist/mimo/mimo                canonical CLI
#   site/dist/mimo/mimo.ps1
#   site/dist/mimo/channel/stable.json
#   site/dist/mimo/templates/api/       project scaffolds for mimo init

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="$(tr -d '[:space:]' < "$ROOT/VERSION")"
OUT_DIR="${OUT_DIR:-$ROOT/dist/mimo-out}"
DIST_ROOT="$ROOT/site/dist/mimo"
CHANNEL_DIR="$DIST_ROOT/channel"
VER_DIR="$DIST_ROOT/lukelang/$VERSION"
ORIGIN="${MIMO_ORIGIN:-https://lukelang.org}"

mkdir -p "$CHANNEL_DIR" "$VER_DIR" "$DIST_ROOT"

# Sync installer scripts into the site tree.
install -m 0644 "$ROOT/tools/mimo/mimo" "$ROOT/site/mimo"
install -m 0644 "$ROOT/tools/mimo/mimo.ps1" "$ROOT/site/mimo.ps1"
install -m 0644 "$ROOT/tools/mimo/install-windows.cmd" "$ROOT/site/install-windows.cmd"
install -m 0644 "$ROOT/tools/mimo/mimo" "$DIST_ROOT/mimo"
install -m 0644 "$ROOT/tools/mimo/mimo.ps1" "$DIST_ROOT/mimo.ps1"
install -m 0644 "$ROOT/tools/mimo/install-windows.cmd" "$DIST_ROOT/install-windows.cmd"
if [ -d "$ROOT/tools/mimo/templates" ]; then
  rm -rf "$DIST_ROOT/templates"
  cp -R "$ROOT/tools/mimo/templates" "$DIST_ROOT/templates"
fi
# Make the site copy look like a downloadable script (deploy chmod will flatten +x).
chmod 0755 "$ROOT/site/mimo" "$DIST_ROOT/mimo" 2>/dev/null || true

TARGETS=(
  linux-x86_64
  linux-i686
  linux-aarch64
  linux-armv7
  darwin-x86_64
  darwin-arm64
  windows-x86_64
  windows-i686
  windows-arm64
)

FOUND=0

for t in "${TARGETS[@]}"; do
  art=""
  if [ -f "$OUT_DIR/luke-${t}.tar.gz" ]; then
    art="$OUT_DIR/luke-${t}.tar.gz"
  elif [ -f "$OUT_DIR/luke-${t}.zip" ]; then
    art="$OUT_DIR/luke-${t}.zip"
  else
    continue
  fi
  base="$(basename "$art")"
  cp -f "$art" "$VER_DIR/$base"
  sha=""
  if [ -f "$OUT_DIR/luke-${t}.sha256" ]; then
    sha="$(awk '{print $1}' "$OUT_DIR/luke-${t}.sha256")"
  elif command -v sha256sum >/dev/null 2>&1; then
    sha="$(sha256sum "$art" | awk '{print $1}')"
  else
    sha="$(python3 -c "import hashlib,sys;print(hashlib.sha256(open(sys.argv[1],'rb').read()).hexdigest())" "$art")"
  fi
  echo "$sha  $base" > "$VER_DIR/luke-${t}.sha256"
  FOUND=$((FOUND + 1))
  echo "  + $t ($sha)"
done

[ "$FOUND" -gt 0 ] || {
  echo "mimo_publish_dist: no artifacts in $OUT_DIR" >&2
  echo "  build some first: scripts/mimo_build_release.sh linux-x86_64" >&2
  exit 1
}

# Write stable.json from whatever landed in VER_DIR.
export VER_DIR ORIGIN
python3 - "$CHANNEL_DIR/stable.json" "$VERSION" <<'PY'
import json, os, sys, datetime
path, version = sys.argv[1], sys.argv[2]
ver_dir = os.environ["VER_DIR"]
origin = os.environ["ORIGIN"]
targets = {}
for name in sorted(os.listdir(ver_dir)):
    if not name.startswith("luke-"):
        continue
    if not (name.endswith(".tar.gz") or name.endswith(".zip")):
        continue
    core = name[len("luke-"):]
    if core.endswith(".tar.gz"):
        target = core[: -len(".tar.gz")]
    else:
        target = core[: -len(".zip")]
    sha_file = os.path.join(ver_dir, f"luke-{target}.sha256")
    sha = open(sha_file, encoding="utf-8").read().split()[0] if os.path.isfile(sha_file) else ""
    targets[target] = {
        "url": f"{origin}/dist/mimo/lukelang/{version}/{name}",
        "sha256": sha,
        "binary": "luke.exe" if target.startswith("windows-") else "luke",
    }
doc = {
    "name": "lukelang",
    "version": version,
    "released": datetime.date.today().isoformat(),
    "mimo_min": "0.3.0",
    "targets": targets,
}
open(path, "w", encoding="utf-8").write(json.dumps(doc, indent=2) + "\n")
print(f"wrote {path} with {len(targets)} targets")
PY

echo "mimo dist ready under site/dist/mimo (version $VERSION, $FOUND targets)"
