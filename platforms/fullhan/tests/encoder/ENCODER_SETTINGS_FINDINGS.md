# Source-owned encoder settings contract

`src/native/capture/fh_encoder_settings.[ch]` contains only defaults,
validation, startup planning and canonical persistence. It does not access a
file, fd, hardware, clock, VMM, live encoder or current stream. Owner integration
selects sensor mode and the established cold-start lifecycle.

Supported startup configurations:

| Setting | Accepted values |
|---|---|
| Dimensions/frame rate |1920x1080 at integer1..15fps;1280x720 at30fps only |
| H264 profile |66 Baseline,77 Main |
| GOP |1..65535frames; application bound |
| Initial QP / I QP / P QP | Each0..51 independently |
| Rate control | FixedQP, mode2; bitrate_kbps must0 |
| Apply timing | Restart required for every changed encoder field |

1080p30 is excluded: retained runtime evidence shows approximately15fps with
abandoned frames, including Baseline's CAVLC path.720p30 is established by the
prior bounded trial.720p15 is excluded from this contract because its full
sensor/VPU combination has not been validated. Existing backing allocations
and VI capacity remain1080p, as in fh_capture; the plan does not invent new
buffer sizes or clock settings. The720p30 plan sets the existing explicit
experimental_fps30 flag; root owns matching sensor-mode selection.

The matching module's pae_enc_set_config accepts only66/77, requires stopped
configuration and regeneratesSPS/PPS. Profile66 disablesCABAC. Existing capture
wire fields are11words, including width,height,GOP,profile,initialQP,packedFPS.
The existing84byte RC request uses mode2 plus packedFPS and I/P quantizers.
QP bounds are driver checks; they do not guarantee bitrate, cadence, desired
visual quality or that every frame fits the owned application buffer.

## Why live changes and bitrate are unsupported

Retained `ipcam_hack_extracted/PAE_KERNEL_EVIDENCE.json` and
`build/rea-pae-kernel-snapshot.json` include matching pae_set_rc_cfg and
rate_control_init. pae_set_rc_cfg checks configured state, calls the RC
initializer, copies80bytes into channel state and regeneratesSPS/PPS. It lacks
an explicit stop check, but that does not establish safe concurrent update,
lock ordering or parameter-set continuity with the native provider. No live
RC ioctl or ownership change is added here.

Nonfixed RC paths additionally configure fh_rc_Init with bitrate-scaled
fields, QP limits, I/P proportions, fluctuation, IP delta, maximum rate,
still-picture settings and controller state. Exact complete mode semantics
and safe presets are not established, so CBR/VBR/arbitrary bitrate requests
return-ENOTSUP. No donor bitrate layout is transplanted.

Original-module profile/CABAC evidence and hash:
`native/rtsp/tests/FPS30_PROFILE_FINDINGS.md`.
PAE layout/RC evidence IDs include
`ev_6da44e9cc1d24488d15a3717dcdd11f2a62e9b8ea7a7b0e573d3ec2e7d7f6b5e`
and`ev_978081dc5bb1a29a2e6f98eeaabdf88325ea00a13b422959d0a9cbe5c07c304a`.

## Persistence and tests

The64byte version1 record is little-endian: magicNENC, version, exact length,
10settings words, two zero reserved words, CRC32 over the first60bytes. CRC
is corruption detection, not authentication. Decode rejects unknown versions,
nonzero reserved words, truncation/trailing bytes, badCRC and invalid settings;
failed operations leave output unchanged. Owner implements authenticated,
atomic file storage and the restart flow.

`test_encoder_settings.c` validates2592 supported combinations against the
actual fh_capture_validate_config, roundtrips each record, verifies canonical
byte order and independent Python-zlib default CRC`e9f93c80`, rejects all512
single-bit corruptions and all truncations, and checks restart/change flags,
unsupported modes and bounds. ASAN/UBSAN and strict staticARMv6 compilation
pass. No device execution or new configuration has been performed.
