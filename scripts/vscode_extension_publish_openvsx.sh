#!/usr/bin/env bash
# Publish the official LukeLang VS Code extension to Open VSX.
#
# Open VSX is the registry Cursor, VSCodium, Gitpod, Theia and the other
# VS Code forks read from — they cannot install from Microsoft's Marketplace.
# Publishing to both is what makes `lukelang` installable everywhere.
#
# Requires OVSX_PAT: an Open VSX access token from https://open-vsx.org/user-settings/tokens
# (sign in with GitHub, then sign the Eclipse Publisher Agreement once).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NAMESPACE="${OVSX_NAMESPACE:-lukelang}"

if [[ -z "${OVSX_PAT:-}" ]]; then
  echo "vscode_extension_publish_openvsx: set OVSX_PAT (token from open-vsx.org/user-settings/tokens)" >&2
  exit 1
fi

bash "$ROOT/scripts/vscode_extension_package.sh"

EXT="$ROOT/tools/vscode/lukelang"
VSIX="$(find "$EXT/dist" -maxdepth 1 -name 'lukelang-vscode-*.vsix' | sort | tail -1)"
if [[ -z "$VSIX" ]]; then
  echo "vscode_extension_publish_openvsx: no .vsix found in $EXT/dist" >&2
  exit 1
fi

cd "$EXT"

# The namespace has to exist before the first publish, and creating one that
# already exists is not an error worth failing the run over.
if npx --yes ovsx create-namespace "$NAMESPACE" -p "$OVSX_PAT" 2>/tmp/ovsx_ns.err; then
  echo "created Open VSX namespace: $NAMESPACE"
else
  if grep -qi 'already exists' /tmp/ovsx_ns.err; then
    echo "Open VSX namespace already exists: $NAMESPACE"
  else
    cat /tmp/ovsx_ns.err >&2
    exit 1
  fi
fi

npx --yes ovsx publish "$VSIX" -p "$OVSX_PAT"
echo "vscode_extension_publish_openvsx_ok=1"
