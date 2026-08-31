#!/usr/bin/env bash
# Build and package luke for one mimo target.
#
#   scripts/mimo_build_release.sh linux-x86_64
#   scripts/mimo_build_release.sh linux-aarch64
#   OUT_DIR=dist/out scripts/mimo_build_release.sh darwin-arm64
#
# Writes:
#   $OUT_DIR/luke-<target>.tar.gz|zip
#   $OUT_DIR/luke-<target>.sha256

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TARGET="${1:?usage: $0 <target>}"
VERSION="$(tr -d '[:space:]' < "$ROOT/VERSION")"
OUT_DIR="${OUT_DIR:-$ROOT/dist/mimo-out}"
VM="$ROOT/vm"
BUILD_DIR="$VM/build-mimo-$TARGET"

mkdir -p "$OUT_DIR"
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

CXX_BIN="${CXX:-}"
CXXFLAGS_EXTRA=()
LDFLAGS_EXTRA=()
EXE_NAME=luke
ARCHIVE_EXT=tar.gz

case "$TARGET" in
  linux-x86_64)
    CXX_BIN="${CXX_BIN:-g++}"
    ;;
  linux-i686)
    CXX_BIN="${CXX_BIN:-i686-linux-gnu-g++}"
    ;;
  linux-aarch64)
    CXX_BIN="${CXX_BIN:-aarch64-linux-gnu-g++}"
    ;;
  linux-armv7)
    CXX_BIN="${CXX_BIN:-arm-linux-gnueabihf-g++}"
    ;;
  darwin-x86_64)
    CXX_BIN="${CXX_BIN:-clang++}"
    CXXFLAGS_EXTRA+=(-target "x86_64-apple-macosx11.0")
    LDFLAGS_EXTRA+=(-target "x86_64-apple-macosx11.0")
    ;;
  darwin-arm64)
    CXX_BIN="${CXX_BIN:-clang++}"
    CXXFLAGS_EXTRA+=(-target "arm64-apple-macosx11.0")
    LDFLAGS_EXTRA+=(-target "arm64-apple-macosx11.0")
    ;;
  windows-x86_64)
    CXX_BIN="${CXX_BIN:-x86_64-w64-mingw32-g++}"
    EXE_NAME=luke.exe
    ARCHIVE_EXT=zip
    ;;
  windows-i686)
    CXX_BIN="${CXX_BIN:-i686-w64-mingw32-g++}"
    EXE_NAME=luke.exe
    ARCHIVE_EXT=zip
    ;;
  windows-arm64)
    # Prefer a native Windows ARM64 toolchain when present (CI).
    if [ -z "$CXX_BIN" ]; then
      if command -v clang++ >/dev/null 2>&1; then
        CXX_BIN=clang++
        CXXFLAGS_EXTRA+=(--target=aarch64-w64-windows-gnu)
        LDFLAGS_EXTRA+=(--target=aarch64-w64-windows-gnu)
      else
        echo "mimo_build_release: no windows-arm64 toolchain on this host" >&2
        exit 2
      fi
    fi
    EXE_NAME=luke.exe
    ARCHIVE_EXT=zip
    ;;
  *)
    echo "unknown target: $TARGET" >&2
    echo "known: linux-x86_64 linux-i686 linux-aarch64 linux-armv7 darwin-x86_64 darwin-arm64 windows-x86_64 windows-i686 windows-arm64" >&2
    exit 1
    ;;
esac

command -v "$CXX_BIN" >/dev/null 2>&1 || {
  echo "mimo_build_release: compiler not found: $CXX_BIN" >&2
  exit 2
}

echo "→ building luke $VERSION for $TARGET with $CXX_BIN"

# Windows/MinGW: the compiler sources use POSIX headers (unistd, fork in DAP).
# Native Windows artifacts are produced on MSYS2/GitHub windows runners where
# those headers exist. Cross-mingw from Linux is best-effort and may fail.
extra_cflags="${CXXFLAGS_EXTRA[*]-}"
extra_ldflags="${LDFLAGS_EXTRA[*]-}"
export CXX="$CXX_BIN"
export CXXFLAGS="-std=c++17 -O2 -Wall -Wextra -Wpedantic -Iinclude ${extra_cflags}"
export LDFLAGS="${extra_ldflags}"

# Isolate objects so parallel target builds do not clobber each other.
make -C "$VM" clean >/dev/null 2>&1 || true
make -C "$VM" -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)" \
  CXX="$CXX_BIN" \
  CXXFLAGS="$CXXFLAGS" \
  LDFLAGS="$LDFLAGS" \
  all

SRC_BIN="$VM/build/luke"
if [ ! -f "$SRC_BIN" ]; then
  # Some toolchains may emit .exe even on make.
  if [ -f "$VM/build/luke.exe" ]; then
    SRC_BIN="$VM/build/luke.exe"
  else
    echo "build failed: $VM/build/luke not found" >&2
    exit 1
  fi
fi

STAGE="$BUILD_DIR/stage"
mkdir -p "$STAGE"
cp "$SRC_BIN" "$STAGE/$EXE_NAME"
chmod +x "$STAGE/$EXE_NAME" 2>/dev/null || true
printf 'LukeLang %s (%s)\nBuilt with mimo release packaging.\n' "$VERSION" "$TARGET" \
  > "$STAGE/README.txt"

ARTIFACT="$OUT_DIR/luke-${TARGET}.${ARCHIVE_EXT}"
rm -f "$ARTIFACT"
(
  cd "$STAGE"
  if [ "$ARCHIVE_EXT" = zip ]; then
    if command -v zip >/dev/null 2>&1; then
      zip -q "$ARTIFACT" "$EXE_NAME" README.txt
    else
      python3 - "$ARTIFACT" "$EXE_NAME" README.txt <<'PY'
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1], "w", zipfile.ZIP_DEFLATED)
for name in sys.argv[2:]:
    z.write(name)
z.close()
PY
    fi
  else
    tar -czf "$ARTIFACT" "$EXE_NAME" README.txt
  fi
)

SUM="$OUT_DIR/luke-${TARGET}.sha256"
if command -v sha256sum >/dev/null 2>&1; then
  (cd "$OUT_DIR" && sha256sum "$(basename "$ARTIFACT")") > "$SUM"
elif command -v shasum >/dev/null 2>&1; then
  (cd "$OUT_DIR" && shasum -a 256 "$(basename "$ARTIFACT")") > "$SUM"
else
  python3 - "$ARTIFACT" "$SUM" <<'PY'
import hashlib, sys, os
path = sys.argv[1]
h = hashlib.sha256(open(path, "rb").read()).hexdigest()
open(sys.argv[2], "w").write(f"{h}  {os.path.basename(path)}\n")
PY
fi

echo "  wrote $ARTIFACT"
echo "  wrote $SUM"
file "$STAGE/$EXE_NAME" 2>/dev/null || true
