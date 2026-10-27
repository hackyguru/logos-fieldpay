#!/usr/bin/env bash
# Build the Field Store harness against the pinned LEZ wallet FFI and run it.
#
#   tests/run.sh                 offline codec checks only
#   tests/run.sh live            organizer + resident end to end on the v0.3 testnet
#
# The live run creates a real token on the public testnet, so it costs a few
# minutes and a little faucet gas. Data dirs live under $WORK (default: a
# fresh temp dir) so it never touches a Basecamp install.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$HERE/../src"
export PATH=/nix/var/nix/profiles/default/bin:$PATH
NIX="nix --extra-experimental-features nix-command --extra-experimental-features flakes"
LEZ_REV=1a003bfbfb89fcf987f62975cd08a401169e1b8c  # v0.3.1

# LEZ's flake builds with rust-overlay's "stable.latest" from its own lock,
# which predates the Rust 1.98 v0.3.1 needs; use a current rust-overlay.
FFI=$($NIX build --no-link --print-out-paths "github:logos-blockchain/logos-execution-zone?rev=$LEZ_REV#wallet" \
    --override-input rust-overlay github:oxalica/rust-overlay)
JSON=$($NIX build --no-link --print-out-paths --impure --expr '(import <nixpkgs> {}).nlohmann_json')
SDK=$($NIX build --no-link --print-out-paths "github:logos-co/logos-cpp-sdk")

# The embedded Token binary, as CMakeLists.txt generates it for the module.
GEN="$HERE/.gen"; mkdir -p "$GEN"
xxd -i < "$SRC/token.bin" > "$GEN/token_elf.inc"

clang++ -std=c++17 -O1 -o "$HERE/harness" "$HERE/harness.cpp" "$SRC/fieldstore_impl.cpp" \
    -I"$SRC" -I"$GEN" -I"$FFI/include" -I"$JSON/include" -I"$SDK/include" \
    -L"$FFI/lib" -lwallet_ffi -Wl,-rpath,"$FFI/lib" \
    -framework Security -framework CoreFoundation -framework SystemConfiguration

"$HERE/harness" codec
[ "${1:-}" = "live" ] || exit 0

WORK="${WORK:-$(mktemp -d)}"
ORG="$WORK/organizer"; RES="$WORK/resident"
mkdir -p "$ORG" "$RES"
org() { FIELDSTORE_DATA_DIR="$ORG" "$HERE/harness" "$@"; }
res() { FIELDSTORE_DATA_DIR="$RES" "$HERE/harness" "$@"; }
jqs() { python3 -c "import json,sys; print(json.load(sys.stdin)$1)"; }

echo; echo "=== live: organizer creates the station ==="
org create "${NAME:-FIELDTEST}" 30000 | tee "$WORK/create.out"
CODE=$(sed -n '/^{/,$p' "$WORK/create.out" | jqs "['station']['code']")
echo "station code: $CODE"

echo; echo "=== live: resident joins ==="
res join "$CODE" | tee "$WORK/join.out"
ADDR=$(sed -n '/^{/,$p' "$WORK/join.out" | jqs "['address']")

echo; echo "=== live: organizer issues 1000 + gas ==="
org issue "$ADDR" 1000 gas

echo; echo "=== live: resident pays 12 publicly ==="
res pay public 12

echo; echo "=== live: resident moves 100 to private ==="
res shield 100

echo; echo "=== live: resident pays 7 privately ==="
res pay private 7

echo; echo "=== live: counter view (organizer) ==="
org state
echo "data dirs kept in $WORK"
