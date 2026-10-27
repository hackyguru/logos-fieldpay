#!/usr/bin/env bash
# Install the freshly built Field Store core + UI into the local LogosBasecamp user dir.
#
# Build first:
#   (cd store-core && nix build 'path:.#lgx-portable' -o result-portable)
#   (cd store-ui   && nix build 'path:.#lgx-portable' -o result-portable \
#        --override-input fieldstore_core path:../store-core)
#
# Core dylibs get the rm + cp + `codesign --force --sign -` treatment: Apple
# Silicon validates every executable page at map time, and nix's ad-hoc
# signature (plus a reused inode from overwriting in place) makes the kernel
# SIGKILL Basecamp with "Code Signature Invalid" on the next launch.
#
# The wallet (keys, recovery words, balances cache) lives in Basecamp's
# module_data for fieldstore_core and is NOT touched by a reinstall.
set -euo pipefail

BASE="$HOME/Library/Application Support/Logos/LogosBasecamp"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CORE_LGX="$HERE/store-core/result-portable/logos-fieldstore_core-module-lib.lgx"
UI_LGX="$HERE/store-ui/result-portable/logos-fieldstore-module.lgx"
VARIANT="darwin-arm64"

for f in "$CORE_LGX" "$UI_LGX"; do
    [ -f "$f" ] || { echo "missing $f — build it first" >&2; exit 1; }
done

echo "==> Quitting Basecamp"
osascript -e 'tell application "LogosBasecamp" to quit' >/dev/null 2>&1 || true
for _ in $(seq 1 20); do pgrep -f LogosBasecamp.bin >/dev/null || break; sleep 0.5; done
pkill -9 -f LogosBasecamp.bin >/dev/null 2>&1 || true
pkill -9 -f logos_host      >/dev/null 2>&1 || true
pkill -9 -f ui-host         >/dev/null 2>&1 || true
sleep 1

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/core" "$TMP/ui"
tar -xzf "$CORE_LGX" -C "$TMP/core"
tar -xzf "$UI_LGX"   -C "$TMP/ui"

echo "==> Installing core -> modules/fieldstore_core"
DEST="$BASE/modules/fieldstore_core"
mkdir -p "$DEST"
for f in "$TMP/core/variants/$VARIANT"/*; do
    name="$(basename "$f")"
    rm -f "$DEST/$name"                 # fresh inode — see header comment
    cp "$f" "$DEST/$name"
    chmod u+w "$DEST/$name"
    case "$name" in
        *.dylib)
            codesign --force --sign - "$DEST/$name" >/dev/null 2>&1
            xattr -c "$DEST/$name" 2>/dev/null || true
            codesign -v "$DEST/$name" || { echo "codesign FAILED for $name" >&2; exit 1; }
            ;;
    esac
    echo "    $name"
done
cp "$TMP/core/manifest.json" "$DEST/manifest.json"
printf '%s' "$VARIANT" > "$DEST/variant"

echo "==> Installing UI -> plugins/fieldstore"
DEST="$BASE/plugins/fieldstore"
mkdir -p "$DEST"
cp -R "$TMP/ui/variants/$VARIANT/." "$DEST/"
# The manifest points `icon` at assets/…, which lives at the package root
# rather than inside the variant — without this the sidebar shows a text tile.
[ -d "$TMP/ui/assets" ] && cp -R "$TMP/ui/assets" "$DEST/assets"
cp "$TMP/ui/manifest.json" "$DEST/manifest.json"
printf '%s' "$VARIANT" > "$DEST/variant"
chmod -R u+w "$DEST"
ls "$DEST" | sed 's/^/    /'

echo "==> Done. Launch with:  open /Applications/LogosBasecamp.app"
