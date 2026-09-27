#!/bin/bash
# install_apple_dtb.sh - Install only Apple Silicon device-tree binaries
#
# Usage: sudo ./install_apple_dtb.sh
#
# This installs *only* the Apple DTBs that update-m1n1 needs
# (t6*.dtb and t81*.dtb), avoiding the full `make dtbs_install`
# which exhausts the small /boot ESP.
#
# The DTBs are placed flat under /boot/dtb-<kernelrelease>/apple/, the
# /boot/dtb symlink is pointed at that directory, update-m1n1 is configured
# (/etc/default/update-m1n1) to take its DTBs from /boot/dtb/apple, and
# m1n1's boot.bin is regenerated. Without this, m1n1 keeps handing the
# distro kernel's DTBs to the custom kernel and e.g. the DCP display
# driver never probes (no DRM device -> no graphical session).
#
# Environment variables:
#   BUILD_DIR          Build directory (default: this script's directory)
#   KERNELRELEASE      Release to install for (default: taken from the
#                      built arch/arm64/boot/Image, which stays correct
#                      even if the tree became -dirty after the build)
#   INSTALL_DTBS_PATH  Destination prefix (default: /boot/dtb-<kernelrelease>)
#   NO_UPDATE_M1N1     If set, do not configure/run update-m1n1

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

if [ -z "${KERNELRELEASE:-}" ]; then
    KERNELRELEASE=$(strings "$BUILD_DIR/arch/arm64/boot/Image" |
                    sed -n 's/^Linux version \([^ ]*\) .*/\1/p' | head -1)
    if [ -z "$KERNELRELEASE" ]; then
        echo "Error: cannot determine kernel release; set KERNELRELEASE" >&2
        exit 1
    fi
fi
if [ ! -d "/lib/modules/$KERNELRELEASE" ]; then
    echo "Error: /lib/modules/$KERNELRELEASE does not exist; install the kernel first" >&2
    exit 1
fi
INSTALL_DTBS_PATH=${INSTALL_DTBS_PATH:-/boot/dtb-$KERNELRELEASE}

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
    dst="$INSTALL_DTBS_PATH/apple/$(basename "$dtb")"

    if [ ! -f "$src" ]; then
        echo "Warning: $src not built, skipping" >&2
        continue
    fi

    install -D -m 0644 "$src" "$dst"
    echo "  $dtb"
    installed=$((installed + 1))
done < "$DTBS_LIST"

echo "Installed $installed Apple DTB(s)"

[ "$installed" -gt 0 ] || { echo "Error: no DTBs installed" >&2; exit 1; }

# Remove the nested layout left behind by older versions of this script.
rm -rf "$INSTALL_DTBS_PATH/arch"

if [ "$INSTALL_DTBS_PATH" = "/boot/dtb-$KERNELRELEASE" ]; then
    if [ -e /boot/dtb ] && [ ! -L /boot/dtb ]; then
        echo "Error: /boot/dtb exists and is not a symlink, not touching it" >&2
        exit 1
    fi
    echo "Pointing /boot/dtb -> dtb-$KERNELRELEASE"
    ln -sfn "dtb-$KERNELRELEASE" /boot/dtb
fi

[ -n "${NO_UPDATE_M1N1:-}" ] && exit 0

# m1n1 stage 2 carries the DTBs; make update-m1n1 (also run by the kernel
# postinst hook) use ours instead of the newest /usr/lib/linux-image-*asahi*.
DEFAULTS=/etc/default/update-m1n1
if ! grep -qs '^DTBS=' "$DEFAULTS"; then
    echo "Configuring $DEFAULTS: DTBS=/boot/dtb/apple/*.dtb"
    echo 'DTBS="/boot/dtb/apple/*.dtb"' >> "$DEFAULTS"
fi

update-m1n1
