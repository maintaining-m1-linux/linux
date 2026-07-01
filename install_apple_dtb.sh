#!/bin/bash
# install_apple_dtb.sh - Install only Apple Silicon device-tree binaries
#
# Usage: sudo ./install_apple_dtb.sh
#
# This installs *only* the Apple DTBs that update-m1n1 needs
# (t6*.dtb and t81*.dtb), avoiding the full `make dtbs_install`
# which exhausts the small /boot ESP.
#
# The DTBs are placed under /boot/dtb-<kernelrelease>/apple so that
# /boot/dtb -> /boot/dtb-<kernelrelease> is no longer a broken symlink
# and update-m1n1 can find them.
#
# Environment variables:
#   BUILD_DIR          Build directory (default: this script's directory)
#   INSTALL_DTBS_PATH  Destination prefix (default: /boot/dtb-<kernelrelease>)

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
BUILD_DIR=${BUILD_DIR:-$SCRIPT_DIR}
DTBS_LIST="$BUILD_DIR/arch/arm64/boot/dts/apple/dtbs-list"

if [ ! -r "$DTBS_LIST" ]; then
    echo "Error: $DTBS_LIST not found." >&2
    echo "Please run 'make dtbs' first (or set BUILD_DIR for out-of-tree builds)." >&2
    exit 1
fi

if [ "$EUID" -ne 0 ]; then
    echo "Error: this script must be run as root (e.g. sudo $0)" >&2
    exit 1
fi

if [ -z "${INSTALL_DTBS_PATH:-}" ]; then
    if [ "$BUILD_DIR" = "$SCRIPT_DIR" ]; then
        KERNELRELEASE=$(make -C "$SCRIPT_DIR" -s kernelrelease)
    else
        KERNELRELEASE=$(make -C "$SCRIPT_DIR" -s O="$BUILD_DIR" kernelrelease)
    fi
    INSTALL_DTBS_PATH="/boot/dtb-$KERNELRELEASE"
fi

echo "Installing Apple DTBs to $INSTALL_DTBS_PATH"

installed=0
while IFS= read -r dtb; do
    [ -n "$dtb" ] || continue

    # Only install Apple Silicon DTBs required by update-m1n1
    case "$dtb" in
        */apple/t6*.dtb|*/apple/t81*.dtb) ;;
        *) continue ;;
    esac

    src="$BUILD_DIR/$dtb"
    dst="$INSTALL_DTBS_PATH/$dtb"

    if [ ! -f "$src" ]; then
        echo "Warning: $src not built, skipping" >&2
        continue
    fi

    install -D -m 0644 "$src" "$dst"
    echo "  $dtb"
    installed=$((installed + 1))
done < "$DTBS_LIST"

echo "Installed $installed Apple DTB(s)"

# If /boot/dtb points to this version's directory (or is broken), keep it valid.
if [ -L /boot/dtb ]; then
    current_link=$(readlink /boot/dtb || true)
    expected_link="dtb-$KERNELRELEASE"
    if [ "$current_link" != "$expected_link" ]; then
        echo "Updating /boot/dtb symlink -> $expected_link"
        ln -sfn "$expected_link" /boot/dtb
    else
        echo "/boot/dtb symlink already correct"
    fi
fi
