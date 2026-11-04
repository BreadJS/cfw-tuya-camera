#!/bin/sh
# Stock read-only ROOTFS invokes precisely this APP entry point. No vendor
# service.sh, daemon, saved CONFIG script, or original Tuya main is executed.
set -eu
unset LD_PRELOAD
umask 077
cd /
runtime=/tmp/camera-runtime
if [ -e "$runtime" ]; then
    echo 'CAMERA_BOOT refusing duplicate/stale runtime' >&2
    exit 1
fi
mkdir -m 700 "$runtime"
cp -R /usr/libexec/camera/. "$runtime/"
# Kernel owns module contents after insmod returns; no APP mapping retained.
for module in ak_rtc mmc_core mmc_block ak_mci ak_saradc ak_i2c ak_pcm ak_isp sensor_mis2006 ak_gpio_keys ak_ion ak_leds ak_uio ak_motor cfg80211 usb-common usbcore ak_hcd rtl8188ftv; do
    name=$module
    [ "$module" != usb-common ] || name=usb_common
    if ! grep -q "^$name " /proc/modules; then
        insmod "/usr/modules/$module.ko" || { echo "CAMERA_BOOT module_failed=$module" >&2; exit 1; }
    fi
    if [ "$module" = ak_rtc ]; then hwclock -s 2>/dev/null || :; fi
done
exec /bin/sh "$runtime/boot"
