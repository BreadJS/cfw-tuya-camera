# Camera firmware

One source project for the LSC Smart Indoor IP Camera and LSC Smart Rotatable
Camera. Version **0.2.0** consolidates the two earlier firmware projects. Select
a board; its profile selects the CPU backend, wiring and available controls.

| Board profile | Chip / CPU | Media | Image/update boundary |
| --- | --- | --- | --- |
| `lsc-smart-indoor` | Fullhan FH8626V100 / ARM1176, ARMv6 | Native MIS2006 backend | Kernel + RAM-root filesystem; preserves bootloader/environment/config |
| `lsc-smart-rotatable` | Anyka AK3918EN080 V300S / AK39EV330, ARM926, ARMv5TE | MIS2006 through an isolated original SDK host | APP mtd7 only; preserves mtd0–6 |

HTTP/authentication, RTSP transport, browser preview, credential storage,
camera name/clock, Telnet login and the dark/red dashboard share maintained
source in `shared/`. Platform modules retain their different media drivers,
picture schemas, GPIO protocols, boot integration and update formats.
The Fullhan backend has no Tuya application. The Anyka backend blocks the
original application's `main` and uses its media SDK code; proprietary code
remains in that transitional host. This is not a universal firmware for all
cameras with these chip markings.

## Quick start

Run commands **inside this folder**. Builds run locally and do not contact or
flash cameras, mount cards, or alter either earlier project.

Requirements: Linux, Python **3.12+**, host C compiler/Make, Node.js, FFmpeg with
libx264, `readelf`, and SquashFS tools with XZ support. Install Python dependencies
with `python3 -m pip install -r requirements.txt` in your preferred environment.
Cross compilers, original firmware inputs and signing keys remain local.

```sh
python3 tools/build.py list
python3 tools/build.py check --board lsc-smart-indoor
python3 tools/toolchain.py --board lsc-smart-indoor
python3 tools/toolchain.py --board lsc-smart-rotatable
```

The toolchain installer verifies the archive's pinned SHA256 and byte count
before extraction. It records compiler paths in ignored `local/toolchains.json`.
Alternatively supply `--cc /absolute/path/to/gcc` to build commands. The two CPUs
use different libc/ABI combinations; do not substitute an arbitrary ARM compiler.

Import owner-provided inputs once using [INPUTS.md](docs/INPUTS.md). Existing
prepared inputs from the earlier projects can be migrated without keeping a
build dependency on those projects:

```sh
python3 tools/import_local.py --backend fullhan --from-project /path/to/custom-firmware
python3 tools/import_local.py --backend anyka --from-project /path/to/custom-firmware-ptz
```

Build an application or complete image:

```sh
python3 tools/build.py application --board lsc-smart-indoor
python3 tools/build.py image --board lsc-smart-indoor
python3 tools/build.py application --board lsc-smart-rotatable
python3 tools/build.py image --board lsc-smart-rotatable --private-key /private/ptz-signing.pem
```

Build SD or web update **candidates** with the two wrappers:

Without `--config`, **both wrappers compile every board configuration** in
`boards/`, one after another. Use `--config NAME` or `--config /path/to/board.json`
to compile just one. `--board` remains a compatibility alias. `--help` explains
options and `--list-configs` lists models without building.

```sh
./compile-sd.sh                     # all configurations, SD packages
./compile-web.sh                    # all configurations, web packages
./compile-sd.sh --config lsc-smart-indoor
./compile-web.sh --config lsc-smart-rotatable --private-key /private/ptz-signing.pem --sequence 20
```

For batch builds, signing keys are selected separately for each configuration.
The wrappers use keys created by `tools/keys.py` in `local/keys/<config>.pem`, or
per-config defaults in ignored `local/build-settings.json`:

```json
{
  "configs": {
    "lsc-smart-indoor": {"private_key": "/private/indoor-signing.pem"},
    "lsc-smart-rotatable": {"private_key": "/private/ptz-signing.pem"}
  }
}
```

Optional per-config settings are `private_key`, `trust_public_key`, `cc`, and
`sequence`; explicit command-line options take precedence. Use `--settings PATH`
for another settings file. Missing required keys/inputs produce build errors;
keys are never generated automatically. Batch builds attempt every config and
return a failure status if any build fails.

Outputs are separated under `output/<board>/`. `work/` is a generated build tree
containing shared code plus only the selected backend. It is replaced at the
next build; edit maintained source, not generated copies. `candidate/` contains
transport ZIPs, hashes and manifests. Candidate filenames end in `.candidate`,
so copying them to a card cannot accidentally trigger automatic installation.
They contain real compiled firmware bytes, but the consolidated build needs its
own exact-image boot evidence before release. [UPDATES.md](docs/UPDATES.md)
explains qualification and final SD/web filenames. Older firmware's successful
boot does not qualify newly compiled bytes.

The default web/Telnet login is `admin` / `camera123`; RTSP uses the same initial
credentials and has independently configurable credentials afterward. Default
addresses are `http://CAMERA_IP:81/`, `rtsp://CAMERA_IP:8554/native` and Telnet
port 23. Recovery Wi-Fi uses a device-derived SSID and password `camera123`.

## Configure a board

Each JSON file in `boards/` specifies its product, SoC, sensor, feature flags,
ports, physical pins and active levels, IR-cut mechanism/timing, reset hold,
PTZ devices/limits/direction and kernel-owned bindings. `soc/` specifies the CPU,
libc/toolchain, media module and GPIO namespace. `project.json` holds the common
version and reproducible build timestamp.

For example, inspect a different IR LED pin/polarity or disable PTZ:

```sh
python3 tools/build.py check --board lsc-smart-indoor \
  --set gpio.ir_led.pin=4 --set gpio.ir_led.active_level=0
python3 tools/build.py application --board lsc-smart-rotatable --disable ptz
```

The same overrides work with image/candidate/release commands. Pin changes
produce a generated C header included by the actual compiler, rather than only
changing a manifest. Driver tests exercise non-default pins/polarity/timing.
Invalid pins, collisions, incompatible drivers and partition changes are rejected.
See [PROFILES.md](docs/PROFILES.md) before changing hardware wiring.

## Develop and share

```sh
python3 tools/test.py                  # both backends; synthetic/loopback only
python3 tools/test.py --board lsc-smart-rotatable
python3 tools/export_source.py         # output/camera-firmware-source.zip
```

The source ZIP is a standalone GitHub-ready source tree. It excludes `local/`,
`output/`, keys, original dumps, toolchains and generated executables. CI runs
profile/driver/shared-media/backend host regressions without proprietary inputs.
Cross builds additionally need the pinned owner-provided assets.

Use [MODULES.md](docs/MODULES.md) to add a board or CPU backend. A new board can
reuse an existing backend when its sensor, drivers and partition assumptions
match. A new chip/sensor requires a real backend port, not just a GPIO table.
Platform-specific unsupported picture controls stay unavailable in the UI.

Project-owned code is MIT licensed; retained third-party notices take precedence
for those files. See [licensing and inputs](docs/INPUTS.md). Vendor firmware,
SDK binaries, kernel modules and toolchains are not included in the public ZIP.
No GitHub repository has been published by these local build commands.
