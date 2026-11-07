#!/usr/bin/env bash
# Compile-checks the Geode/GD layer against the real GD 2.2081 bindings on Linux,
# and verifies every $modify hook will actually hook on Windows.
#
# Why: the real build needs Windows + MSVC/clang-cl, and some mistakes don't fail
# it at all (a hook with a slightly wrong signature silently never runs). This
# catches those on any Linux box in a couple of minutes.
#
# How:
#   1. clone Geode SDK + bindings + header deps at the refs in versions.env
#   2. build Geode's codegen, generate headers for Android64 and Win64
#   3. clang -fsyntax-only every src/ file against the Android64 headers
#      (same API surface as Windows; parseable with host clang + libc++)
#   4. run hook_check.py against the Win64 codegen data
#
# Needs: clang, libc++-dev, libc++abi-dev, libgles-dev, cmake, git, python3.
# Usage: scripts/header-check.sh [work-dir]   (default: build-headercheck)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(realpath -m "${1:-$ROOT/build-headercheck}")"
# shellcheck source=versions.env
source "$ROOT/scripts/versions.env"
mkdir -p "$WORK/deps"

# mod.json must declare the same versions this script verifies against.
python3 - "$ROOT/mod.json" "$GEODE_REF" "$GD_VERSION" <<'PY'
import json, sys
mod = json.load(open(sys.argv[1]))
geode, gd = sys.argv[2].lstrip("v"), sys.argv[3]
problems = []
if mod.get("geode") != geode:
    problems.append(f'mod.json "geode" is {mod.get("geode")!r}, versions.env says {geode!r}')
gds = mod.get("gd", {})
for plat, ver in (gds.items() if isinstance(gds, dict) else [("*", gds)]):
    if ver not in (gd, "*"):
        problems.append(f'mod.json "gd.{plat}" is {ver!r}, versions.env says {gd!r}')
if problems:
    print("version mismatch:\n  " + "\n  ".join(problems)); sys.exit(1)
print(f"== versions consistent: Geode {geode}, GD {gd}")
PY

fetch() { # dir url ref
    local dir="$1" url="$2" ref="$3"
    if [ -d "$dir/.git" ] && [ "$(git -C "$dir" rev-parse HEAD)" = "$(git -C "$dir" rev-parse "$ref^{commit}" 2>/dev/null || true)" ]; then
        return
    fi
    rm -rf "$dir"
    git init -q "$dir"
    git -C "$dir" remote add origin "$url"
    # shallow-fetch tags/branches; short SHAs need a full fetch
    if ! git -C "$dir" fetch -q --depth 1 origin "$ref" 2>/dev/null; then
        git -C "$dir" fetch -q origin
    fi
    git -C "$dir" -c advice.detachedHead=false checkout -q "$ref" 2>/dev/null \
        || git -C "$dir" -c advice.detachedHead=false checkout -q FETCH_HEAD
    echo "  $(basename "$dir") @ $(git -C "$dir" rev-parse --short HEAD)"
}

echo "== fetching sources (GD $GD_VERSION, Geode $GEODE_REF)"
fetch "$WORK/geode"    https://github.com/geode-sdk/geode.git       "$GEODE_REF"
fetch "$WORK/bindings" https://github.com/geode-sdk/bindings.git    "$BINDINGS_REF"
fetch "$WORK/deps/result" https://github.com/geode-sdk/result.git   "$RESULT_REF"
fetch "$WORK/deps/json"   https://github.com/geode-sdk/json.git     "$JSON_REF"
fetch "$WORK/deps/ntf"    https://github.com/geode-sdk/nontype_functional.git "$NTF_REF"
fetch "$WORK/deps/fmt"    https://github.com/fmtlib/fmt.git         "$FMT_REF"
fetch "$WORK/deps/asp2"   https://github.com/dankmeme01/asp2.git    "$ASP_REF"
fetch "$WORK/deps/arc"    https://github.com/dankmeme01/arc.git     "$ARC_REF"
fetch "$WORK/deps/tulip"  https://github.com/geode-sdk/TulipHook.git "$TULIP_REF"

SDK_VER="$(cat "$WORK/geode/VERSION")"
[ -d "$WORK/bindings/bindings/$GD_VERSION" ] || { echo "bindings has no $GD_VERSION folder"; exit 1; }

echo "== building codegen"
cmake -S "$WORK/bindings/codegen" -B "$WORK/codegen-build" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$WORK/codegen-build" -j"$(nproc)" >/dev/null
CODEGEN="$WORK/codegen-build/Codegen"

echo "== generating $GD_VERSION headers (Android64 for parsing, Win64 for hook data)"
rm -rf "$WORK/gen-android" "$WORK/gen-win"
"$CODEGEN" Android64 "$WORK/bindings/bindings/$GD_VERSION" "$WORK/gen-android" --sdk-version "$SDK_VER" >/dev/null
"$CODEGEN" Win64     "$WORK/bindings/bindings/$GD_VERSION" "$WORK/gen-win"     --sdk-version "$SDK_VER" >/dev/null

G="$WORK/geode/loader/include"
D="$WORK/deps"
CXXFLAGS=(
    -std=c++23 -fsyntax-only -stdlib=libc++ -Wno-everything
    -D__ANDROID__ -DANDROID -D__aarch64__
    -DGEODE_GD_VERSION="$GD_VERSION" -DGEODE_COMP_GD_VERSION="${GD_VERSION/./}"
    "-DGEODE_GD_VERSION_STRING=\"$GD_VERSION\"" "-DGEODE_MOD_ID=\"fwlab.frame-window-lab\""
    -DPROJECT_NAME=FrameWindowLab
    "-Dnear=" "-Dfar=" # <windows.h> defines these as empty macros; catch identifiers named near/far
    -include "$ROOT/scripts/stubs/ndk_alias.hpp" -include cstdarg
    -I"$WORK/gen-android" -I"$WORK/bindings/bindings/include"
    -I"$G" -I"$G/Geode/cocos/include" -I"$G/Geode/cocos/extensions" -I"$G/Geode/fmod"
    -I"$D/result/include" -I"$D/json/include" -I"$D/ntf/include" -I"$D/fmt/include"
    -I"$D/asp2/include" -I"$D/arc/include" -I"$D/tulip/include"
    -I"$ROOT/scripts/stubs" -I"$ROOT/src"
)

compile() { clang++ "${CXXFLAGS[@]}" "$1" 2>&1 | grep -E "error|fatal" || true; }

echo "== harness self-test"
cat > "$WORK/selftest_ok.cpp" <<'EOF'
#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
using namespace geode::prelude;
struct $modify(T, PlayLayer) { void postUpdate(float dt) { PlayLayer::postUpdate(dt); (void)m_isPaused; } };
EOF
sed 's/(void)m_isPaused;/(void)m_definitelyNotAMember;/' "$WORK/selftest_ok.cpp" > "$WORK/selftest_bad.cpp"
if [ -n "$(compile "$WORK/selftest_ok.cpp")" ]; then
    echo "harness broken: a valid file failed:"; compile "$WORK/selftest_ok.cpp" | head; exit 1
fi
if [ -z "$(compile "$WORK/selftest_bad.cpp" | grep m_definitelyNotAMember)" ]; then
    echo "harness broken: an invalid member was not reported"; exit 1
fi
echo "  ok (valid code passes, a bogus member is rejected)"

echo "== syntax-checking src/ against GD $GD_VERSION headers"
fail=0
while IFS= read -r f; do
    out="$(compile "$f")"
    if [ -n "$out" ]; then
        echo "  FAIL ${f#"$ROOT"/}"; echo "$out" | sed 's/^/    /' | head -20; fail=1
    else
        echo "  ok   ${f#"$ROOT"/}"
    fi
done < <(find "$ROOT/src" -name '*.cpp' | sort)

echo "== checking settings used by the code exist in mod.json"
python3 "$ROOT/scripts/settings_check.py" || fail=1

echo "== verifying hooks against Win64 bindings"
python3 "$ROOT/scripts/hook_check.py" --self-test "$WORK/gen-win/Geode/CodegenData.json" | tail -1
python3 "$ROOT/scripts/hook_check.py" "$WORK/gen-win/Geode/CodegenData.json" "$ROOT/src" || fail=1

exit $fail
