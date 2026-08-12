#!/usr/bin/env bash
set -euo pipefail

DRIVER_REPOSITORY="https://github.com/svpcom/rtl8812au.git"
DRIVER_COMMIT="20bcaf511f159bfd8f435f7117b82056fc453572"
SCRIPT_DIRECTORY=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PATCH_FILE="$SCRIPT_DIRECTORY/rtl8812au_v5.2.20_apfpv.patch"
RUBY_DRIVER="/home/radxa/ruby/drivers/88XXau-radxa.ko"
KERNEL_RELEASE=$(uname -r)
KERNEL_DRIVER="/lib/modules/$KERNEL_RELEASE/kernel/drivers/net/wireless/88XXau.ko"
BUILD_DIRECTORY=$(mktemp -d)
MODINFO=$(command -v modinfo || true)

if [ -z "$MODINFO" ] && [ -x /sbin/modinfo ]; then
    MODINFO=/sbin/modinfo
fi

trap 'rm -rf "$BUILD_DIRECTORY"' EXIT

if [ ! -d "/lib/modules/$KERNEL_RELEASE/build" ]; then
    echo "ERROR: Kernel headers are missing for $KERNEL_RELEASE."
    exit 1
fi

git clone --quiet "$DRIVER_REPOSITORY" "$BUILD_DIRECTORY/rtl8812au"
cd "$BUILD_DIRECTORY/rtl8812au"
git checkout --quiet "$DRIVER_COMMIT"
git apply --check "$PATCH_FILE"
git apply "$PATCH_FILE"
make -j"${MAKE_JOBS:-4}"

BUILT_DRIVER="$BUILD_DIRECTORY/rtl8812au/88XXau_wfb.ko"
if [ ! -f "$BUILT_DRIVER" ]; then
    echo "ERROR: Driver build did not produce $BUILT_DRIVER."
    exit 1
fi
if [ -z "$MODINFO" ]; then
    echo "ERROR: modinfo is not available."
    exit 1
fi
if [ "$("$MODINFO" -F name "$BUILT_DRIVER")" != "88XXau_wfb" ]; then
    echo "ERROR: Built module has an unexpected internal name."
    exit 1
fi
case "$("$MODINFO" -F vermagic "$BUILT_DRIVER")" in
    "$KERNEL_RELEASE "*) ;;
    *)
        echo "ERROR: Built module does not match kernel $KERNEL_RELEASE."
        exit 1
        ;;
esac

# Ruby copies this packaged file whenever its user-triggered driver reset runs.
# Updating both locations keeps that reset path from restoring the old module.
sudo install -m 0644 "$BUILT_DRIVER" "$RUBY_DRIVER.new"
sudo mv "$RUBY_DRIVER.new" "$RUBY_DRIVER"
sudo install -d -m 0755 "$(dirname "$KERNEL_DRIVER")"
sudo install -m 0644 "$BUILT_DRIVER" "$KERNEL_DRIVER.new"
sudo mv "$KERNEL_DRIVER.new" "$KERNEL_DRIVER"
sudo depmod -a "$KERNEL_RELEASE"

echo "Installed patched Ruby RTL8812AU driver:"
sha256sum "$BUILT_DRIVER" "$RUBY_DRIVER" "$KERNEL_DRIVER"
echo "The patched kernel module will be loaded after reboot."
