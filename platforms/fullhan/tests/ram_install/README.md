# Single-process RAM installer checks

`src/ota/ram_install.c` is the production orchestration layer over the existing
authenticated plan, durable boot-confirmation journal and gated Linux NOR
backend. `ram_install_main.c` accepts only `--apply-inherited`, with descriptors:

| FD | Purpose |
| --- | --- |
|3| Existing private persistent update-state directory |
|4| Existing private RAM staging directory |
|5| Same open file description holding the exclusive named state flock |
|6| Read-only private32-byte regular RAM copy of the trusted public key |
|7| Separate private700 RAM progress directory |

The source API is `ram_install_apply_inherited()`. It borrows these descriptors,
duplicates the plan/state-lock descriptors before opening any MTD device, and
fully re-verifies the capsule through `ota_apply_plan_open_locked()`. Inputs
are trusted service handoff descriptors, never browser-supplied paths or a
user-asserted isolation flag.

Production lease checks require:

- root privileges, exact supported kernel/page size, single installer thread;
- root/PID1/helper executables in RAM, and the exact root-owned0400
  `/.camera-boot-origin` value `NOR_ROOT_V1\n` on RAM storage;
- unique normal `root=/dev/mtdblock4 rootfstype=squashfs` boot arguments,
  rejecting SD/recovery/alternate-init arguments;
- actual JFFS2 configuration storage on31:5 and update state on that same
  filesystem; staging/key/progress remain in RAM;
- still-owned named exclusive flock and unchanged authenticated plan;
- no3/4 target mounts, process mappings, roots, executables or working
  directories backed by either target, and no raw MTD/block/watchdog FDs in
  another process; only this backend's exact identity-checked target FDs are
  permitted in the installer itself;
- live vendor watchdog-pause readback and exact kernel text fingerprints;
- the exact80-byte `mtdchar_close` implementation supporting the selected
  Linux4.9 final-release synchronization policy;
- matching six-partition map, plus the backend's independent device ownership,
  sysfs attributes, NOR geometry, flags, identity and exact range checks.

The caller must serialize its own threads while sharing the state flock and
disable configuration/update mutations for the duration of the child operation.
The helper itself never forks, duplicates MTD descriptors or spawns processes.
Separate synchronization descriptors are fresh opens made by the backend;
the installer checks its single-thread/process/descriptor isolation throughout.

Only after preconditions pass does it durably publish `ota_boot_prepare()`.
Only after that succeeds may the backend open the selected devices and execute
erase/write/full readback of kernel3 and/or rootfs4. Bootstrap0, environment1,
bootloader2 and configuration5 cannot be selected by this backend. The only
configuration writes are its bounded transaction journal via JFFS2; that
partition is never erased or programmed through the raw backend.

`install-progress.json` is bounded below768bytes and published atomically from
an owned fixed temporary filename inside FD7. Its final phase is
`written_reboot_pending`. A successful result requires backend finalization
and a final plan guard. Neither success nor failure changes accepted sequence,
claims boot confirmation, deletes the capsule/journal or reboots the camera.
`journal_prepared` means known durable preparation; a prepare failure may still
have left a recoverable journal. `may_have_changed` is conservative once an
erase phase has been authorized. Callers must inspect the result even on error.

CLI exit protocol:0 means verified success;10 means a known failure before
this attempt could mutate firmware;11 means possibly changed or unknown.
Invalid usage is2. Signals and other exit codes are not a pristine-failure
attestation. Exit10 does not prove an older journal is clean: the parent must
remember whether a journal pre-existed and may discard a matching journal only
after a first known-pristine failure under its authenticated cancellation
policy. The core itself never discards any journal.

## Why the detached RAM design avoids media teardown

The old apply warning assumed processes or libraries still depended on a NOR
root. Here PID1 first copies and verifies the **complete** runtime into tmpfs,
pivots root and detaches the old container. Subsequent executables, libraries,
module/firmware files and paths come from RAM. The lease independently checks
for remaining selected-partition mounts, mappings and file descriptors.

The running Linux kernel and loaded module code are already in DRAM. The
validated media transport/ARC firmware and DMA buffers use reserved DRAM above
the Linux36MiB bank, rather than either SPI NOR partition. Rewriting detached
kernel/rootfs storage therefore does not remove the memory used by live media
DMA. A dangerous close/reopen media sequence is not needed to isolate those
NOR targets. Hardware watchdog pause removes the old timer-owner dependency.
This reasoning is specific to the pinned hardware/runtime and its verified RAM
boot contract; this helper does not make arbitrary NOR-backed firmware safe.

There is no A/B or power-loss rollback claim. On a write failure the existing
RAM runtime and retained capsule/journal are the repair path; preserve the
independent bootloader/UART/SD recovery path before a first physical deployment.

## Source/fake tests only

From the project root:

```sh
python3 custom-firmware/tests/ram_install/test_install.py
SANITIZE=1 python3 custom-firmware/tests/ram_install/test_install.py
```

Tests generate signed fixtures and exercise the actual planner, boot-journal
preparation, executor, Linux backend and installer orchestration. Every device
operation is routed to the pre-existing complete syscall-fake table backed by
an8MiB heap array. The fake-only entry is absent unless
`CAMERA_RAM_INSTALL_TEST` is explicitly compiled. It requires a non-NULL
complete fake table and cannot silently choose physical syscalls.

Coverage includes full and single-image plans; protected partitions;
precondition-refusal wiring; journal/device-activation failures; short I/O and
EINTR; erase/write/readback/lease/synchronization/final-close failures; retained
capsule/pending/accepted state; bounded atomic progress; and the actual normal
boot-argument parser rejecting recovery, duplicates and oversized strings.
The environment callback deliberately models production precondition outcomes;
host tests do not establish actual camera mount/MMIO/process state. Those
production conditions require read-only hardware preflight and review before
any physical apply. No real MTD or camera invocation is performed by these tests.

Production builds must omit `CAMERA_RAM_INSTALL_TEST` and define
`CAMERA_WATCHDOG_PROBE_LIBRARY` when linking the reusable watchdog checker.
