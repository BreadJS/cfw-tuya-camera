# Exact-camera native OSD evidence

Evidence class: REVERSE_CONFIRMED for the static contracts below; host/source
checks for the implementation. Hardware/encoded visual acceptance is pending.
The REA skill guided this bounded investigation; the shared session was used
exclusively and closed after exporting evidence. No runtime target was executed.

Primary binary identities:

| Target | SHA256 |
| --- | --- |
| matching dgiot | `bdc0fd42a67fc2c51ae6caff380b9d3592953198f6f8faad27e5d1c7aea83714` |
| matching isp.ko | `790a18d2eaa21f0b38d74a535849f72a44e78cd097db4693ea7e869000631567` |

REA/Ghidra12.1.4 exported full records to
`/tmp/camerhack-osd-dgiot.evidence.json` and
`/tmp/camerhack-osd-kernel.evidence.json`; matching snapshots are in the same
temporary directory with `-snapshot.json` suffixes. These are workstation caches,
not durable external evidence locators or source dependencies. Concise contracts
and IDs are retained here; promote/compress selected complete records into the
project's evidence store if long-term replay is required. Complete evidence records
are now retained as `docs/evidence/osd-dgiot.rea.json.gz` and
`docs/evidence/osd-kernel.rea.json.gz` relative to the firmware project root.

## Exact GraphV2 request

REA `ev_3f7e6834aa965f8b3de00bd3f00916b113d8fab32566c45dbe5479566bc36335`
decompiled matching dgiot `FH_VPSS_SetChnGraphV2` at `0xe99f0..0xe9c87`.
The wrapper accepts channels0/1, selects `channel+1`, builds the header below,
sets word18 to zero, copies128 palette words and calls
`ioctl(existing_isp_fd,0xc448696d,wire)`.

The prior exact-module `VPU_KERNEL_EVIDENCE.json` establishes that `vpu_control`
dispatches that exact request but copies only0x24c bytes, despite0x448 being
encoded in the request number. New kernel REA record
`ev_3e8acfd2e6ff9b9913a386f376f85597d0ea37272c3ab4e0ad8e1d0452200752`
confirms `vpu_set_logov2` at `0x17338..0x177c7` consumes/caches147 words.

| Wire word | Meaning |
| --- | --- |
| 0 | selector1 for main channel0; selector2 for channel1; selector0 global |
| 1 | slot; channel slots0..3 |
| 2 | enabled; zero takes the disable branch before geometry checks |
| 3 | pixel bit depth; stock text uses2 |
| 4 | VMM physical bitmap address |
| 5..8 | color/alpha controls; stock text leaves these zero |
| 9 | stock text/logo mode2 |
| 10..12 | additional format flags, zero in the selected stock text path |
| 13,14 | width,height |
| 15,16 | x,y; hardware coordinate packing divides these by2 |
| 17 | byte stride; checked divisible by8 |
| 18 | color bank index0/1, not a palette entry |
| 19..146 | 128 palette words |

`ev_e7a7a04903b628897403d87687028f33cfa49f645d5cc3ad55aea695c075eb55`
contains exact kernel `vpu_set_logov2_reg` and enable dispatch. The driver writes
the selected main-channel plane's settings and physical address, thereby
compositing before encoded output rather than in a browser.

`ev_8e8269395d962a265383c7dcd060be2bf92e81f43ce278030cce05f16ae60b1f`
contains `vpu_get_pixel_depth` and `vpu_set_logo_color`. Depth2 maps to the indexed
hardware mode; channel palettes for depths1/2/4 write exactly2^depth entries,
starting from wire19. No palette writes are guessed outside that path.

## Exact stock packing, colors and retained updates

`ev_884af44484dc986b7696c38ebd55d6d22f543daffd0d361446370693fcb0ca9a`
decompiles matching glyph writer `0xca06c..0xca8ff` and address helper
`0xc9fe4..0xca06b`. It proves byte offset `y*stride+(x>>2)` and per-pixel shift
`2*(x&3)`: leftmost pixel occupies the low two bits, four pixels per byte.
The outline pass sets index1 around foreground indices greater than1.
It copies the resulting glyph bytes into the retained bitmap, without allocating
or freeing a replacement per clock tick.

`ev_8536b1039e1a326f2da308ed6c9f2fff4dc1d5012d1db4a8c988fd77f3832711`
decompiles matching OSD initialization `0xcb824..0xcc2e7`. It constructs palette
words from RGBA bytes as `A<<24|R<<16|G<<8|B`, creates2-bit text buffers and
invokes the same glyph writer. Thus the source-owned palette policy selects
transparent0, opaque-black`0xff000000` outline, opaque-white`0xffffffff` text
and a transparent unused fourth entry. Exact rendering still requires visual
acceptance on the current live encoded stream.

The rendering/update loop at `0xcaf80..0xcb4ef`, record
`ev_13119ff5869d75b596815a33c80381f4c7ee4072f3c26af2c9bc52a19f2b9bbd`,
updates changed glyphs through that writer. This supports retaining a single
mapped buffer and updating it in place. It does not establish an atomic
frame-boundary swap or tear-free timestamp transition.

## Donor boundary and implementation policy

The semantic donor
[native GraphV2 helper](https://github.com/ArthurKoba/openipc-divinus/blob/50e3e300bb92b609e230ab9af1cf712f49c95a39/src/hal/full/native/osd/fh8626_graphv2.c)
has a different shape:274 words with the palette starting at word18. That tail
layout must not be copied into this camera. Its newer per-update new-VMM-fd,
replace and release lifecycle also does not match our proven whole-zone VMM
ownership contract. The implementation here was written against exact camera
records, not by transplanting those assumptions.

The existing `legacy_vmm.h` explicitly records no individual free. OSD reserves
one4KiB block from the existing media owner's session and keeps it through
disable/re-enable and failures. The bitmap is418x26, indexed2-bit, stride112;
the spare allocation bytes remain zero. No fresh media or VMM descriptor is
opened or closed by this component. Root owns runtime integration, time conversion,
settings persistence, build changes and eventual hardware validation.

The public clock API accepts cached civil time or preformatted23-character text.
It never changes process-global TZ/locale and uses fixed English weekday names.
The web controller can therefore choose Europe/Amsterdam or UTC without racing
another timestamp formatter. Camera name rendering is a separate UI setting.

Remaining acceptance gates: actual placement and alpha/color visibility in the
decoded RTSP stream, enable/disable without stream disruption, DST/timezone
conversion in the root controller, persistent web settings and long-running
buffer/stream behavior. No hardware request has been submitted by this subtask.
