#!/usr/bin/env bash
# Dev convenience launcher: Homebrew's Qt keeps its platform plugins under
# <prefix>/share/qt/plugins instead of the location Qt's own heuristics
# expect, so an unbundled dev build can't find the "cocoa"/"xcb" platform
# plugin unless QT_PLUGIN_PATH points there explicitly. Packaged release
# builds (macdeployqt/windeployqt/linuxdeploy) embed plugins alongside the
# binary and won't need this.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
QT_PREFIX="$(brew --prefix qtbase 2>/dev/null || brew --prefix qt 2>/dev/null || true)"

if [[ -n "$QT_PREFIX" && -d "$QT_PREFIX/share/qt/plugins" ]]; then
  export QT_PLUGIN_PATH="$QT_PREFIX/share/qt/plugins"
fi

exec "$SCRIPT_DIR/build/directpdf" "$@"
