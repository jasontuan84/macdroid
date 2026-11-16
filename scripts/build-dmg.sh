#!/bin/bash
# Builds a Release MacDroid.app and packages it as releases/MacDroid-<version>.dmg.
# The DMG window shows the app next to an Applications shortcut so users can
# drag it in to install. The volume and the .dmg file use the app icon.
#
# Usage: scripts/build-dmg.sh
# Env:   QT_PREFIX (default ~/Qt/6.12.0/macos), QT_TOOLS (default ~/Qt/Tools),
#        SIGN_IDENTITY (default "-" = ad-hoc signature)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
QT_PREFIX="${QT_PREFIX:-$HOME/Qt/6.12.0/macos}"
QT_TOOLS="${QT_TOOLS:-$HOME/Qt/Tools}"
SIGN_IDENTITY="${SIGN_IDENTITY:--}"
export PATH="$QT_TOOLS/CMake/CMake.app/Contents/bin:$QT_TOOLS/Ninja:$PATH"

VERSION="$(sed -n 's/^project(MacDroid VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
BUILD="$ROOT/build/release"
APP="$BUILD/MacDroid.app"
OUT_DIR="$ROOT/releases"
DMG="$OUT_DIR/MacDroid-$VERSION.dmg"
VOLNAME="MacDroid $VERSION"
WORK="$(mktemp -d)"
MNT="/Volumes/$VOLNAME"
trap 'hdiutil detach "$MNT" -quiet 2>/dev/null || true; rm -rf "$WORK"' EXIT

echo "==> Building MacDroid $VERSION (Release)"
# Remove the old bundle (macdeployqt output) BEFORE configuring: CMake writes
# Contents/Info.plist at configure time, and the build does not recreate it.
rm -rf "$APP"
cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$QT_PREFIX" >/dev/null
cmake --build "$BUILD"
[ -f "$APP/Contents/Info.plist" ] || { echo "error: Info.plist missing from $APP" >&2; exit 1; }

echo "==> Bundling Qt, libmtp and libusb"
"$QT_PREFIX/bin/macdeployqt" "$APP" -verbose=1
# Every non-system library must now come from inside the bundle.
if otool -L "$APP/Contents/MacOS/MacDroid" "$APP"/Contents/Frameworks/*.dylib | grep -E '^\s+/(opt|usr/local|Users)/'; then
    echo "error: the bundle still links libraries outside the app" >&2
    exit 1
fi

echo "==> Signing (${SIGN_IDENTITY})"
if [ "$SIGN_IDENTITY" = "-" ]; then
    # Ad-hoc: no hardened runtime, otherwise library validation rejects the
    # bundled frameworks ("different Team IDs") and the app cannot start.
    codesign --force --deep --sign - "$APP"
else
    # Developer ID: hardened runtime + timestamp, ready for notarization.
    codesign --force --deep --options runtime --timestamp --sign "$SIGN_IDENTITY" "$APP"
fi
codesign --verify --deep --strict "$APP"

echo "==> Preparing the DMG window"
# Window layout (points). Keep in sync with scripts/dmg-background.swift.
WIN_W=660; WIN_H=420; ICON_Y=170; APP_X=165; APPS_X=495
STAGE="$WORK/stage"
mkdir -p "$STAGE/.background"
cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
swift "$ROOT/scripts/dmg-background.swift" "$WORK/bg1.png" $WIN_W $WIN_H 1 $ICON_Y
swift "$ROOT/scripts/dmg-background.swift" "$WORK/bg2.png" $WIN_W $WIN_H 2 $ICON_Y
# Multi-resolution TIFF so the background is sharp on Retina displays.
tiffutil -cathidpicheck "$WORK/bg1.png" "$WORK/bg2.png" -out "$STAGE/.background/background.tiff" >/dev/null

hdiutil create -quiet -srcfolder "$STAGE" -volname "$VOLNAME" -fs HFS+ -format UDRW -ov "$WORK/rw.dmg"
if [ -d "$MNT" ]; then
    echo "error: $MNT is already mounted; eject it first" >&2
    exit 1
fi
hdiutil attach -quiet -readwrite -noverify -noautoopen "$WORK/rw.dmg"
SetFile -a V "$MNT/.background"

# Finder lays out the window (needs Automation permission for Finder once).
# Hidden items are moved far outside the window in case Finder shows hidden files.
osascript <<OSA
tell application "Finder"
    set theDisk to disk "$VOLNAME"
    open theDisk
    delay 1
    set win to container window of theDisk
    set current view of win to icon view
    set toolbar visible of win to false
    set statusbar visible of win to false
    set bounds of win to {200, 120, 200 + $WIN_W, 120 + $WIN_H}
    set opts to icon view options of win
    set arrangement of opts to not arranged
    set icon size of opts to 128
    set text size of opts to 13
    set background picture of opts to file ".background:background.tiff" of theDisk
    set position of item "MacDroid.app" of win to {$APP_X, $ICON_Y}
    set position of item "Applications" of win to {$APPS_X, $ICON_Y}
    repeat with hiddenName in {".background", ".fseventsd", ".Trashes"}
        try
            set position of item (hiddenName as text) of win to {1200, 1200}
        end try
    end repeat
    update theDisk without registering applications
    delay 1
    close win
end tell
OSA
rm -rf "$MNT/.fseventsd" "$MNT/.Trashes"
# Volume icon: set AFTER the Finder step, which removes .VolumeIcon.icns and
# clears the volume's custom-icon flag.
cp "$APP/Contents/Resources/MacDroid.icns" "$MNT/.VolumeIcon.icns"
SetFile -a V "$MNT/.VolumeIcon.icns"
SetFile -a C "$MNT"
sync
hdiutil detach -quiet "$MNT"

echo "==> Compressing"
mkdir -p "$OUT_DIR"
hdiutil convert -quiet "$WORK/rw.dmg" -format ULFO -ov -o "$DMG"

# Give the .dmg file itself the app icon (shown in Finder on this Mac;
# custom file icons are lost when the file is uploaded or downloaded).
osascript -l JavaScript - "$APP/Contents/Resources/MacDroid.icns" "$DMG" <<'EOF' >/dev/null
ObjC.import('AppKit');
function run(argv) {
    const icon = $.NSImage.alloc.initWithContentsOfFile(argv[0]);
    return $.NSWorkspace.sharedWorkspace.setIconForFileOptions(icon, argv[1], 0);
}
EOF

echo "==> Done: $DMG"
