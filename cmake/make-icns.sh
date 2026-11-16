#!/bin/sh
# Builds the macOS app icon from flat artwork: make-icns.sh <artwork.png> <output.icns>
# The artwork is first placed on a macOS-style squircle (compose-icon.swift).
set -eu
src="$1"; out="$2"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
swift "$(dirname "$0")/compose-icon.swift" "$src" "$work/icon_1024.png"
set_dir="$work/AppIcon.iconset"
mkdir -p "$set_dir"
for size in 16 32 128 256 512; do
    sips -z $size $size "$work/icon_1024.png" --out "$set_dir/icon_${size}x${size}.png" >/dev/null
    double=$((size * 2))
    sips -z $double $double "$work/icon_1024.png" --out "$set_dir/icon_${size}x${size}@2x.png" >/dev/null
done
iconutil -c icns "$set_dir" -o "$out"
