# Post-boot confirmation component

`../../src/ota/boot_confirm.[ch]` adds bounded install-journal preparation and
post-boot acceptance. It does not open, erase or write an MTD device. Installed
bytes come only through a caller-supplied read callback restricted to partition3
(kernel) and partition4(rootfs). A production adapter must independently pin
read-only devices and validate their geometry and identity.

Run host tests from the project root:

```sh
python3 custom-firmware/tests/boot_confirm/test_boot_confirm.py
```

Tests use disposable RFC8032 signing material, synthetic nonbootable kernel and
filesystem headers, private local temporary directories and ASan/UBSan. They
exercise filesystem durability boundaries by controlled failures; they do not
prove real JFFS2 power-loss behavior or successful camera installation.

## Installer integration

1. Retain a fully authenticated `ota_apply_plan` and its exclusive state lock.
   Serialize other service threads sharing that open-file-description lock.
2. Parse the current Linux `boot_id` into16bytes. Call `ota_boot_prepare` before
   **every** physical apply attempt, including retries in later boots. It fully
   re-verifies the capsule and persists `install-pending`; it never advances
   `accepted`. If any durability step fails, do not erase/write firmware.
3. Keep the ordinary signed `pending` header until post-boot confirmation.
   Do not treat staging, completed writes or a self-reported release number as
   proof of a successful boot.

## Post-boot integration

Run `ota_boot_confirm` **before** `ota_state_open` or `control_update_init`.
Open the existing private state directory and exact named `lock`, then provide:

- A trusted public key, new kernel boot ID and read-only installed-image reader.
- A local installed-firmware-origin attestation: verified NOR-root PID1
  bootstrap, expected boot arguments and runtime/config mounts. Reject SD,
  recovery and trial boots. A different boot ID alone is insufficient.
- Actual camera/video/audio health observed in this boot. Neither attestation
  may come directly from an HTTP parameter or version string.

The module verifies the journal's signature and sequence, rebuilds the exact
canonical vendor payload from installed image bytes, checks image headers/CRCs,
and compares its full BLAKE2b-512 digest with the signed header. It also checks
the complete erased-FF tail of every selected partition against the executor's
block policy, so an interrupted trailing erase cannot count as a finished
installation. Only then does
it atomically replace `accepted`, sync it, remove `pending`, sync the directory,
remove `install-pending`, and sync again. No journal returns0 and never confirms
a merely staged update. Confirmed/recovered returns1. Failure returns-1 and
may leave an already-renamed `accepted`; retry confirmation before opening the
ordinary staging service.

An interruption after `accepted` advances can leave `accepted == pending`.
Ordinary `ota_state_open` intentionally rejects that state. The confirmation
component authenticates and validates installed bytes again, then finishes the
cleanup. It never decreases `accepted` or accepts a conflicting pending header.
Because accepted has already been durably advanced on that recovery path,
cleanup can proceed despite a later media-health failure; it still requires
installed boot origin and complete authenticated installed-byte/tail readback,
and never advances the sequence. This preserves access to repair management.

Release/close the confirmation lock only when handing off to the ordinary
staging service. Staging storage must be fresh RAM after a reboot; this component
does not remove an unexpectedly surviving RAM `pending.cap`. Explicit discard
or a different update after failed installation must also manage the bounded
`install-pending` journal under the same lock; do not silently delete it before
accepted-commit recovery has been attempted.

## Journal format

All integers below are little endian. Total size is exactly240bytes, mode0600,
one link, owned by the service UID inside its mode0700 state directory.

| Offset | Bytes | Field |
| --- | --- | --- |
|0|8|`FHCBOOT1`|
|8|4|Format version1|
|12|4|Record count1 or2|
|16|16|Installer kernel boot ID|
|32|192|Original signed capsule header|
|224|8|First image type and byte length|
|232|8|Second type and length, or zero when unused|

Map fields and boot ID are not separately signed. Private file ownership binds
the boot ID to the trusted installer. The image map remains untrusted until its
canonical reconstruction matches the payload digest in the signed header.
Types must be unique ascending2/3, lengths must fit their exact partitions, and
their total must equal the signed payload size. No full capsule needs to fit in
the320KiB persistent configuration partition.
