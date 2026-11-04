# Unified build validation — 2026-11-04

Version 0.2.0 was built locally for both supported boards. No camera was contacted
or flashed during this consolidation. These are compiled and inspected
candidates, not newly hardware-qualified releases.

Passed checks:

- Real ARMv6/musl static Fullhan application/helpers and complete kernel/RAM-root
  firmware container, within the reviewed rootfs budget.
- Real ARMv5TE/uClibc1.0.28 Anyka application/login/clock, static signed-update
  installer and complete 5 MiB APP partition image, within its budget.
- Both backends additionally compile with every optional feature disabled.
- Both web/SD transports generated from their respective backend modules;
  actual capsule Ed25519 signatures, board identity, sequence, length and
  BLAKE2b checked; Fullhan SD layout/CRC inspected; Anyka SD member allowlist,
  APP bytes and transport MD5/footer checked.
- Fullhan verified RAM-root archive extraction, corruption/truncation rejection,
  packed file/symlink hashes, private asset permissions and read-only source
  normalization using ASan/UBSan tests.
- Profile/export/publication unit tests, non-default GPIO/polarity/pulse tests
  against actual driver code, and strict collision/partition/binding checks.
- Backend regressions: reset long-press handling, Telnet login, Fullhan image
  controls/illumination, Anyka smooth PTZ/lease/reversal, media ownership,
  sensor timing, picture/encoder controls, audio sample clocks and management.
- Shared RTCP clocks, constrained-link complete-AU recovery, client failure
  isolation, MP4 preview bit-exact decode/variable PTS/reconnect, display storage.
- UI picture-save adapters, PTZ pointer/touch/keyboard/cancellation behavior,
  disabled PTZ, and generated dashboard JavaScript syntax.
- Public source ZIP extracted into a separate `/tmp` directory; explicit local
  owner assets/compiler mapping copied in; BOTH full image and SD/web candidate
  builds succeed there without either earlier source project.
- Fresh Anyka hostapd/libnl-tiny cross-build through the public network setup
  command using pinned source inputs; compiler/tool lookup works independently
  of the generated application compiler wrapper.

Current canonical candidate hashes (also recorded in each output manifest):

| Board/image | SHA256 |
| --- | --- |
| Indoor kernel | `4bb984f46c9d0347f9156bd4b91bb78cdc39fa27054a5b2b8279e3726f3392c5` |
| Indoor RAM-root container | `e554ea4925153d48068c71654450616b93124e1301bd54303198f2c70d049bbc` |
| Rotatable APP | `3ebd55d927d124faeebc60d9ca5b405e2c68d36e85d599adc5b8062b5a5b57e7` |

The Anyka media shared object matches between the original workspace and the
separate extracted checkout. Its static installer's compiler metadata includes
build-path information, so complete APP hashes can differ across checkout paths.
Qualification always binds to the actual image hash, never an assumed
cross-directory reproducibility claim. Fixed build timestamps alone do not
remove toolchain metadata differences.

GPIO/DTB validation establishes software configuration agreement, not electrical
or optical proof on a new board. Changed firmware composition and changed wiring
still need an exact-image boot test before release. Anyka retains proprietary SDK
code in its guarded host, and only the two named sensor/board revisions are
supported. Previously recorded legacy hardware checks do not qualify these bytes.
