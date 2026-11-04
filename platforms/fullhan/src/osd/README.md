# Encoded timestamp OSD

This component prepares a native FH8626 timestamp plane before H.264 encoding.
It is not a browser/CSS overlay. Host tests and ARM compilation pass; encoded
visual acceptance on this camera is still pending. No camera ioctl, allocation,
media restart or flash write was performed during this component's development.

The default position is (16,16). The 418x26 bitmap contains
`YYYY-MM-DD Thu HH:MM:SS` using small white glyphs with a black outline and
transparent background. Its indexed2-bit rows occupy112 bytes; one reserved
4KiB VMM block holds the entire plane. The font is source-owned and needs no
external font library or firmware resources.

## Integration

Use `runtime.h` from the existing media owner, serialized with its other ISP
control calls. Supply the already-open ISP descriptor and existing legacy VMM
session. The module never opens/closes a device or VMM session, frees an
individual allocation, resets the VMM zone or restarts media.

1. Start with `struct camera_osd osd = CAMERA_OSD_INITIALIZER`.
2. Call `camera_osd_init(&osd,isp_fd,&vmm,&io,width,height)` once after media
   startup. The callbacks check the existing exclusive owner and submit packets
   to that existing ISP descriptor. Initialization reserves/maps its block and
   starts disabled, issuing no GraphV2 request.
3. Supply the root controller's cached, timezone-converted `struct tm` to
   `camera_osd_update_time`, or its exact23-character ASCII timestamp to
   `camera_osd_update_text`, once per second. No global TZ/locale/clock is changed.
4. Apply settings with `camera_osd_set_enabled(&osd,enabled)`. Enabling requires
   a rendered timestamp; disabling sends the exact disable packet and retains
   the buffer for later re-enable.

`camera_osd_io.submit` has four parameters. When adapting the existing
five-parameter `fh_capture_linux_ioctl`, pass `(uint8_t *)wire` and
`CAMERA_GRAPHV2_WORDS * sizeof(uint32_t)` as its final arguments. Do not cast
between incompatible function pointer signatures.

The module reuses the same memory for every update; unchanged timestamps skip
the copy. It stages rendering locally and then updates the mapped buffer in
place, matching the stock glyph writer's retained-buffer model. There is no
frame-sync fence: a changing second can briefly produce a mixed transition
frame. No old buffer is freed or reused for another owner, so this does not
create an address-retirement/DMA-lifetime hazard. Mapping must satisfy the
existing legacy VMM contract's uncached/coherent CPU/DMA access requirement.

## Failure and lifetime

Owner checks precede callbacks and memory updates. Allocation/map failures
retain known allocation tracking, with no rollback/reset. An ioctl error may
have had effects and makes the fault sticky; `attempted` records the need for
disable even when an enable returned an error. `camera_osd_disable` can retry
while the exclusive lease remains valid. A successful disable retains the block
and does not erase an earlier fault; re-enable then requires owner recovery.

There is no standalone OSD buffer destroy/free. The matching legacy driver has
no per-block free and closing a separate VMM session can reset the whole zone.
Backing memory remains valid until the owner quiesces the whole pipeline and
performs its existing VMM teardown. Reinitializing a live/faulted OSD context
is rejected rather than consuming another slot silently.

## Offline checks

Compile each test with `timestamp.c`, `graphv2.c` as needed; `test_runtime.c`
also links `runtime.c` and supplies injected VMM functions instead of device
operations. All compile with `-std=c11 -Wall -Wextra -Werror -pedantic`; host
ASan/UBSan checks pass. Tests cover English weekdays, date validation, buffer
boundaries, outline coverage, bit packing, exact packet offsets, one-time
allocation, unchanged-text handling, toggles, failed enable followed by disable,
lost lease and failed mapping with allocation retained. Production objects also
compile with the available ARMv6 musl cross compiler. These checks do not prove
encoded placement, color/opacity, driver activation or ongoing stream quality.

See `EVIDENCE.md` for exact binary identities, REA records and ABI differences
from the donor source.
