# Configuration reset transaction

Run `python3 custom-firmware/tests/reset_config/test_reset.py`. Fifteen host tests use private disposable `/tmp` fixtures and ASan/UBSan. No camera device or existing user credentials are accessed. Tests cover durable request publication, every reset removal/durability boundary, idempotent recovery, borrowed and independently held real flock descriptors, install-journal exclusion, exact-name preservation, malformed settings, safe symlink unlinking, unsafe markers/state, and refusal of the production CLI on an unsupported host.

`camera_config_reset_request` queues a durable private `reset-request` marker. It leaves running settings untouched. The caller must hold its updater mutex, supply actual installer busy state, and block all mutating APIs and future installation after a successful request until reboot. A nonnegative OTA lock descriptor is borrowed, verified against the named private `update-state/lock`, retained locked, and never closed by this module. `-1` acquires a local nonblocking lock; missing private state/lock may be created without creating or changing an accepted sequence.

`camera_config_reset_apply` runs once at early boot, before network, hardware and service settings writers. It acquires/verifies the same OTA lock and refuses any `install-pending` entry. It removes exactly these names:

- `wpa_supplicant.conf`
- `credentials.conf`
- `settings.conf`
- `recovery.conf`
- `illumination.conf`
- `indicator.conf`

Strict temporary names from the actual writers are also removed: `credentials.tmp.<digits>`, `recovery.conf.tmp.<digits>`, `{settings,illumination,indicator}.tmp.<digits>.<digits>`, `.wifi-<digits>-<digits>.tmp`, and the reset module's `.reset-request.new`. This is exact grammar recognition, not a shell glob. Similar factory names remain. Unlink never follows symlinks and never recursively deletes directories. Corrupt contents in own regular files are still cleared. A directory/special file occupying an own slot fails closed with the reset marker retained.

After removing all own names, apply fsyncs the configuration directory before removing the marker, then fsyncs again. A power interruption before completion leaves either a retryable marker or already durable removals. This is a logical configuration reset, not a forensic erase; it does not touch the trust key, accepted/pending/install journal contents, Wi-Fi hardware calibration or other factory files. Pending OTA identities are preserved, including conflicts that might independently disable the updater.

Build the standalone production CLI from `src/reset/config_reset.c` with `-DCAMERA_CONFIG_RESET_MAIN`. Install at `/usr/libexec/camera/config-reset`, then call `/usr/libexec/camera/config-reset --apply || exit 1` in `rcS` after mounting JFFS2 and creating/chmodding `/etc/conf/camera`, before hardware/network/service startup. The CLI insists on root, a RAM root, the private exact `NOR_ROOT_V1` origin marker on RAM, and JFFS2 `/etc/conf` device 31:5. It refuses SD/RAM trials. No GPIO, reboot, direct MTD or flash erase operations exist in the module or CLI. Hardware button handling and reboot remain caller responsibilities.

Static ARMv6 EABI5 soft-float CLI compilation also passed with `-static -mcpu=arm1176jzf-s -mfloat-abi=soft -Wall -Wextra -Werror`. The resulting host-prepared binary was not executed or uploaded to the camera.
