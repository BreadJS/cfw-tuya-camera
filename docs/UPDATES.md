# SD and web update builds

`compile-web.sh` and `compile-sd.sh` select the transport and invoke the same
board/module build system. `tools/build.py candidate --target both --board ...`
can build both transports from one image. Neither command writes flash.

| Backend | Web artifact | Automatic SD artifact | Flash scope |
| --- | --- | --- | --- |
| Fullhan | `camera-firmware.cap`, Ed25519 signed envelope | `linux.bin`, bootloader CRC transport | Reviewed kernel/rootfs slots only |
| Anyka | `camera-app.cap`, Ed25519 signed envelope | `firmware.bin`, original SD tar/footer transport | APP mtd7 only |

SD transports use the original updater's CRC/MD5 checks; they do not inherit web
signature verification. Protected partitions and configuration remain preserved
by the reviewed layout. Anyka's original SD script may remove stock ISP config;
the own APP includes its reviewed sensor profile. Neither backend has atomic
single-bank power-loss rollback.

A candidate build compiles the full image and produces these bytes with
`.candidate` appended to filenames. Its manifest says `flash_ready:false` and
records the complete profile hash and exact canonical image SHA256 values.
This prevents accidental auto-flashing while the new composition awaits its own
boot test. The unchanged legacy firmware's validation files are historical input
records, not boot evidence for the unified build.

After performing a controlled boot test with suitable recovery access, record
its evidence as a JSON file outside the public source:

```json
{
  "schema": 1,
  "board_profile": "lsc-smart-indoor",
  "profile_sha256": "COPY_FROM_CANDIDATE_MANIFEST",
  "images": {
    "kernel": "COPY_EXACT_SHA256",
    "rootfs": "COPY_EXACT_SHA256"
  },
  "boot_id": "ACTUAL_LINUX_BOOT_UUID",
  "checks": {"video": true, "audio": true, "web": true, "network": true}
}
```

For Anyka, `images` contains only `app` with that canonical partition image hash.
The UUID must be the tested boot's `/proc/sys/kernel/random/boot_id`. `checks`
attest to advancing video, working audio when enabled, reachable authenticated
web UI and networking. This is an operator's runtime evidence record, not a
builder-generated substitute for testing. Test Wi-Fi persistence/recovery,
buttons, IR and PTZ on changed board wiring before deployment as appropriate.

```sh
python3 tools/build.py release --board lsc-smart-indoor --target both \
  --private-key /private/indoor-signing.pem --sequence 20 \
  --boot-evidence /private/tested-boot.json
```

Use the same profile overrides and trust anchor as in the tested candidate.
The release command rebuilds and refuses evidence if image bytes, board or
profile hash differ. It publishes normal installer filenames only after matching
qualification. An invalid or missing evidence record never silently becomes a
release. There is no automatic camera contact or physical-install claim.

For an existing custom installation, upload the correct board's `.cap` in the
web interface, review verification, then select Install. The sequence must exceed
the camera's accepted sequence, and the signing key must match its installed
anchor. Uploading alone does not install. Keep power connected during install.

For a qualified SD bootstrap, extract the board's SD ZIP to the root of a FAT32
card with one MBR partition, verify `SHA256SUMS`, safely eject, power the camera
off, insert the card and power it on. Fullhan requires `linux.bin`; Anyka requires
`firmware.bin`. Do not put both boards' packages on one card. After the dashboard,
video/audio and normal reboot work, remove the automatic SD file before reusing
the card. A fixed waiting time or LED alone does not prove installation completed.
