#!/usr/bin/env bash
# Verify the files produced by `cmake --install` without touching a live audio
# server, display or tray host.
set -euo pipefail

usage() {
    echo "usage: $0 <install-prefix>" >&2
    exit 2
}

if [[ $# -ne 1 ]]; then
    usage
fi

prefix=$1
failures=0

fail() {
    echo "install-smoke: $*" >&2
    failures=$((failures + 1))
}

require_file() {
    if [[ ! -f "$prefix/$1" ]]; then
        fail "missing file: $1"
    fi
}

require_executable() {
    if [[ ! -x "$prefix/$1" ]]; then
        fail "missing executable: $1"
    fi
}

require_executable "bin/pipeduck"
require_file "share/applications/pipeduck.desktop"
require_file "share/man/man1/pipeduck.1"

desktop_file="$prefix/share/applications/pipeduck.desktop"
icon_name="pipeduck"
if [[ -f "$desktop_file" ]]; then
    icon_name=$(sed -n 's/^Icon=//p' "$desktop_file" | head -n1)
    if [[ -z "$icon_name" ]]; then
        fail "desktop entry has no Icon= field"
        icon_name="pipeduck"
    fi
fi

for size in 16 22 24 32 48 64 128 256 512; do
    require_file "share/icons/hicolor/${size}x${size}/apps/${icon_name}.png"
    require_file "share/icons/hicolor/${size}x${size}/apps/${icon_name}-symbolic.png"
done
require_file "share/icons/pipeduck/pixmap/pipeduck-48.argb"
require_file "share/icons/pipeduck/pixmap/pipeduck-on-48.argb"

for document in README.md LICENSE NOTICE CHANGELOG.md; do
    require_file "share/doc/pipeduck/$document"
done

if [[ -x "$prefix/bin/pipeduck" ]]; then
    version_output=$("$prefix/bin/pipeduck" --version)
    if [[ "$version_output" != pipeduck\ * ]]; then
        fail "unexpected --version output: $version_output"
    fi
    if ! "$prefix/bin/pipeduck" --help >/dev/null; then
        fail "--help failed"
    fi
fi

if [[ $failures -ne 0 ]]; then
    echo "install-smoke: $failures check(s) failed" >&2
    exit 1
fi

echo "install-smoke: all checks passed"
