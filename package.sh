#!/usr/bin/env bash
# Builds DirectPDF.app as a self-contained macOS bundle: configures + builds
# with CMake, runs macdeployqt to embed the Qt frameworks and the non-Qt
# dependencies (poppler-qt6, qpdf) it pulls in transitively, then patches in
# the "cocoa" platform plugin by hand — Homebrew's Qt keeps platform plugins
# under <prefix>/share/qt/plugins instead of the location macdeployqt's own
# heuristics search (see run.sh), so it never gets copied automatically and
# the bundle fails to start without it.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
APP="$BUILD_DIR/DirectPDF.app"

QT_PREFIX="$(brew --prefix qtbase 2>/dev/null || brew --prefix qt 2>/dev/null)"
MACDEPLOYQT="$QT_PREFIX/bin/macdeployqt"
COCOA_PLUGIN="$QT_PREFIX/share/qt/plugins/platforms/libqcocoa.dylib"

if [[ ! -x "$MACDEPLOYQT" ]]; then
  echo "error: macdeployqt not found at $MACDEPLOYQT" >&2
  exit 1
fi
if [[ ! -f "$COCOA_PLUGIN" ]]; then
  echo "error: cocoa platform plugin not found at $COCOA_PLUGIN" >&2
  exit 1
fi

cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" >/dev/null
cmake --build "$BUILD_DIR" -j"$(sysctl -n hw.ncpu)"

rm -rf "$APP/Contents/Frameworks" "$APP/Contents/PlugIns"
"$MACDEPLOYQT" "$APP"

mkdir -p "$APP/Contents/PlugIns/platforms"
cp "$COCOA_PLUGIN" "$APP/Contents/PlugIns/platforms/"

# Re-sign everything ad hoc as a whole: macdeployqt's own codesign pass only
# covers what it copied, and copying the cocoa plugin in afterwards leaves
# the bundle's resource-seal manifest stale, which fails a --deep verify.
codesign --force --deep -s - "$APP"
codesign --verify --deep --strict "$APP"

echo "Built $APP"
echo "Run it with: open \"$APP\""
echo "Install it with: cp -R \"$APP\" /Applications/"
