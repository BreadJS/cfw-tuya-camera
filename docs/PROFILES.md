# Board and SoC profiles

`boards/<name>.json` is the authoritative product/wiring configuration.
`tools/profiles.py` validates it, and `tools/workspace.py` emits:

- `src/board/camera_board_config.h`, forced into every target compilation;
- `config/board-profile.json`, the full effective configuration;
- `config/board-public.json`, dashboard capabilities and indicator modes;
- launcher templates with configured service ports;
- `build-composition.json`, maintained source origins/hashes.

The authenticated `/api/board` endpoint exposes the effective profile identity,
SoC, capabilities, ports and profile hash. The page embeds the same metadata.
Default Fullhan IR-cut uses GPIO19/20 (300 ms pulse, flip1), IR LED3, red2/green6
(active low), and reset23 (active low, five seconds held then released).
Default Anyka IR-cut uses GPIO6/67 (latched select/enable), IR LED7 and status42,
reset5 (evdev KEY_POWER116), pan8/41/79/34 and tilt81/80/9/68. PTZ calibration
uses 4096/2048 steps of travel, 600 steps/s and a 400 ms renewable hold lease. Renewing hold
does not restart motor movement; release, lost lease or reversal stops movement.

The available optional features are audio, timestamp, rotation, picture controls,
IR-cut, IR LED, status LED, reset and PTZ. Core HTTP/RTSP/preview/Wi-Fi/hotspot/
updates/clock/Telnet services are explicit required service modules in v1 of the
profile schema. `--disable` applies to optional capabilities; it is not a general
link-time dead-code elimination interface.

GPIO-driven hardware uses configured pins/polarity. Anyka LEDs and reset can
instead use verified kernel LED-class/evdev bindings. Motor pins are owned by
`ak_motor`; arbitrary user-space writes cannot reassign them. `kernel_bindings`
records the actual DTB node/property, pins and polarity/event identity.
The build parses the imported DTB and rejects disagreement. Changing a motor
pin or kernel LED/key binding therefore requires a separately reviewed compatible
DTB/module port. The stock Anyka updater preserves that DTB; changing JSON alone
cannot install a new one. A GPIO LED/reset transport may use an otherwise free
pin; do not leave stale fields from a different driver type in its JSON object.

Numeric GPIO ranges are driver address ranges, not proof that every pin is
exposed, wired safely or usable on a new board. Pin muxing, sensor/radio buses,
power, voltage and physical wiring need board evidence. Known kernel-owned pins
are reserved even when their feature is disabled. The physical IR-cut polarity
and timing must match the mechanism. No new board should inherit another board's
flash qualification merely because validation or compilation succeeds.

The reviewed flash partition map is owned by the backend module and matched
against the board profile. A config edit cannot silently redirect flash writes.
A new geometry requires porting and testing the updater, immutable hashes and
packager together. Keep a distinct compatibility ID when hardware is not update
compatible; it must fit the update envelope's31-character identity limit.
