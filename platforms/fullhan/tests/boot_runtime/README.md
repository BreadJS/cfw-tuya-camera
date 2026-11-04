# Production boot-confirmation adapter

The adapter is in `../../src/ota/boot_runtime.[ch]`. It creates no worker,
forks no process, and opens no physical write FD. The parent application owns
thread scheduling and real video/audio-health observation.

```c
int needed = camera_boot_confirmation_needed(state_path);
/*0: ordinary updater can initialize.1: defer it and schedule confirmation.
 * -1: unsafe/inaccessible journal metadata; fail closed, not "no journal". */

struct ota_boot_result result;
int rc = camera_boot_confirm_installed(key_path, state_path,
                                      actual_media_healthy, &result,
                                      error, sizeof error);
/* Call this BEFORE control_update_init/ota_state_open.
 *0: no journal,1: confirmed/recovered,-1: retry/report failure. */
```

On-boot control/web/stream services may run while updater initialization is
deferred. Health must come from actual current video/audio progress, never an
HTTP parameter or image version string. For interrupted already-accepted
cleanup, `actual_media_healthy=0` is supported by the core; this path revalidates
the installed bytes and never advances the accepted sequence.

The adapter locally requires:

- Root service, pinned kernel release/build, RAM root and RAM PID1 executable/root.
- `/.camera-boot-origin` is a root-owned, one-link, mode0400 RAM file on the
  current root filesystem, containing exactly `NOR_ROOT_V1\n`.
- One exact `root=/dev/mtdblock4` argument, SquashFS if specified, and no
  alternate init, writable root, SD-test, recovery, rescue or single-user flags.
- Actual JFFS2 configuration on device31:5, with the state directory on that
  same device and an owned private state lock.
- The kernel-backed clock probe validates exact code/mapping fingerprints,
  two stable paused watchdog-clock readings and absent watchdog registration.
- A canonical current kernel boot UUID, distinct from the installer boot UUID
  as subsequently enforced by the boot-confirmation core.

It then pins `/dev/mtd3` and `/dev/mtd4` using `O_RDONLY|O_NOFOLLOW|O_CLOEXEC`.
Named and open character-device identities must match90:6/90:8, root ownership
and safe permissions. All six `/proc/mtd` partitions must match the exact8MiB
layout. Only `MEMGETINFO` and `MEMGETREGIONCOUNT` metadata ioctls are issued;
NOR type, image budget, uniform64KiB erase size,1-byte writesize, no OOB and
valid NOR capability flags are required. Read-only logical protection is allowed.
Rootfs/kernel sysfs name, dev, size and offset attributes must match the pinned
layout. There is no guessed-offset or missing-attribute fallback.

Each actual read rechecks the named/open FD identity and access mode. Before
returning the final byte of each selected full partition, it rechecks geometry,
sysfs attributes and the complete map. The boot-confirmation core then validates
the signature, installed image bytes and all erased-FF tails before changing
private configuration state. No physical erase/write/unlock ioctl exists in
this adapter.

On any error, leave ordinary updater initialization deferred until the
confirmation/recovery result is resolved. Accepted may already have been
atomically advanced before a cleanup or descriptor-close error; inspect the
result and retry. Do not replace the trusted public key, reset `accepted` or
delete `install-pending` to bypass a failure.

## Host validation boundary

```sh
python3 custom-firmware/tests/boot_runtime/test_boot_runtime.py
```

Seven ASan/UBSan host tests exercise strict boot arguments/UUIDs, fixed NOR
identity and geometry predicates, private journal metadata, symlink/hardlink
rejection and unsupported-host refusal. A watchdog stub aborts if the unsupported
host ever reaches hardware probing. No test reads a real device, accesses UART,
or attempts firmware installation. Positive production-origin/MTD/sysfs behavior
and actual JFFS2 durability still require the camera's controlled install/boot
test. Full static ARMv6 linking is checked separately.
