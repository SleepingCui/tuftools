#!/usr/bin/env bash
# Build tuftools. Output: build/tuftools
#
# Usage:
#   ./build.sh                  # cmake build, Release (default)
#   ./build.sh cmake --debug    # cmake build, Debug
#   ./build.sh direct           # plain single compiler command, no cmake
#   ./build.sh --clean          # wipe build/ first
#
# Environment:
#   CXX    compiler to use (default: g++, then clang++)
#   JOBS   parallel jobs for the cmake build (default: all cores)

set -euo pipefail

usage() {
    sed -n '2,12p' "$0"
}

backend="cmake"
debug=0
clean=0

for arg in "$@"; do
    case "$arg" in
        cmake|direct) backend="$arg" ;;
        -Debug|--debug|-d) debug=1 ;;
        -Clean|--clean|-c) clean=1 ;;
        -h|--help) usage; exit 0 ;;
        *)
            echo "unknown argument: $arg (try --help)" >&2
            exit 2
            ;;
    esac
done

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src="$here/src"
third_party="$here/third_party"
out="$here/build"
exe="$out/tuftools"
config="Release"
[ "$debug" -eq 1 ] && config="Debug"

if [ ! -f "$third_party/nlohmann/json.hpp" ]; then
    echo "vendored headers missing: expected $third_party/nlohmann/json.hpp" >&2
    exit 1
fi

if [ "$clean" -eq 1 ] && [ -d "$out" ]; then
    echo "removing $out"
    # Data files live next to the executable, so keep them across --clean.
    saved="$(mktemp -d)"
    for name in costs.json difficulties.json; do
        if [ -f "$out/$name" ]; then cp -p "$out/$name" "$saved/$name"; fi
    done
    rm -rf "$out"
    mkdir -p "$out"
    for name in costs.json difficulties.json; do
        if [ -f "$saved/$name" ]; then cp -p "$saved/$name" "$out/$name"; fi
    done
    rm -rf "$saved"
fi
mkdir -p "$out"

resolve_cxx() {
    if [ -n "${CXX:-}" ]; then
        command -v "$CXX" >/dev/null 2>&1 || { echo "CXX='$CXX' not found" >&2; exit 1; }
        printf '%s\n' "$CXX"
        return
    fi
    if command -v g++ >/dev/null 2>&1; then printf 'g++\n'; return; fi
    if command -v clang++ >/dev/null 2>&1; then printf 'clang++\n'; return; fi
    echo "no C++ compiler found (install g++ or clang++)" >&2
    exit 1
}

# libcurl is found via pkg-config when available, otherwise -lcurl.
curl_flags() {
    if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists libcurl; then
        pkg-config --cflags --libs libcurl
    else
        printf '%s\n' "-lcurl"
    fi
}

if [ "$backend" = "direct" ]; then
    cxx="$(resolve_cxx)"
    flags=(-std=c++17 -Wall "-I$third_party")
    if [ "$debug" -eq 1 ]; then
        flags+=(-O0 -g)
    else
        flags+=(-O2)
    fi
    if [ -n "${CPPFLAGS:-}" ]; then
        read -r -a extra_cpp <<< "$CPPFLAGS"
        flags+=("${extra_cpp[@]}")
    fi
    ldflags=()
    if [ -n "${LDFLAGS:-}" ]; then
        read -r -a ldflags <<< "$LDFLAGS"
    fi
    sources=()
    while IFS= read -r file; do
        sources+=("$file")
    done < <(find "$src" -maxdepth 1 -name '*.cpp' | sort)
    # Word splitting is intentional: pkg-config output is a flag list.
    # shellcheck disable=SC2086
    curl=$(curl_flags)
    echo "compiling with $cxx (direct backend)"
    # ${ldflags[@]+...} keeps bash 3.2 (macOS) happy under set -u when empty.
    # shellcheck disable=SC2086
    "$cxx" "${flags[@]}" -o "$exe" "${sources[@]}" ${ldflags[@]+"${ldflags[@]}"} $curl
else
    command -v cmake >/dev/null 2>&1 || { echo "cmake not found. Install CMake, or run: ./build.sh direct" >&2; exit 1; }
    cxx="$(resolve_cxx)"
    jobs="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"

    # A build tree is tied to the compiler that configured it.
    cache="$out/CMakeCache.txt"
    if [ -f "$cache" ]; then
        previous="$(sed -n 's/^CMAKE_CXX_COMPILER:FILEPATH=//p' "$cache" | head -1)"
        previous="${previous%%;*}"
        if [ -n "$previous" ] && [ "$previous" != "$cxx" ] && [ "$(basename "$previous")" != "$(basename "$cxx")" ]; then
            echo "build/ was configured for '$previous', but this run selected '$cxx'. Run: ./build.sh --clean" >&2
            exit 1
        fi
    fi

    echo "configuring with cmake ($config)"
    cmake -S "$here" -B "$out" -DCMAKE_BUILD_TYPE="$config" -DCMAKE_CXX_COMPILER="$cxx"
    echo "compiling with $cxx"
    # Drop the artifact first so a stale non-cmake build is not treated as up to date.
    rm -f "$exe"
    cmake --build "$out" --config "$config" --parallel "$jobs"
fi

if [ ! -x "$exe" ]; then
    echo "expected artifact not found: $exe" >&2
    exit 1
fi
echo "built $exe"
