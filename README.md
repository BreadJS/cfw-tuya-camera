# Camera firmware

Custom firmware for two LSC cameras, built from one project. Version **0.2.0**.

Browse the [camera catalogue](https://breadjs.github.io/cfw-tuya-camera/) to see
supported cameras, photos, hardware details and build configurations.

| Camera | Config |
| --- | --- |
| LSC Smart Indoor IP Camera — Fullhan FH8626V100 | `lsc-smart-indoor` |
| LSC Smart Rotatable Camera — Anyka AK39EV330 | `lsc-smart-rotatable` |

The Anyka version currently uses an isolated vendor SDK host.

## 1. Open the project folder

Run all commands below from the `camera-firmware` folder:

```sh
cd /path/to/camera-firmware
```

**Already set up on this computer? Skip to step 5.**

## 2. Install the build tools

Use Linux with Python **3.12 or newer**. On Ubuntu 24.04:

```sh
sudo apt install build-essential cmake binutils squashfs-tools ffmpeg nodejs python3-venv
python3 -m venv .venv
. .venv/bin/activate
python3 -m pip install -r requirements.txt
```

Activate `.venv` again when opening a new terminal.

Install the bundled compiler for your camera, or both for SD batch builds.
Both pinned toolchain archives are included in `toolchains/`; installation
verifies their size and SHA-256 and extracts them into a local cache. It works
offline and never downloads a toolchain from GitHub or another server.

```sh
python3 tools/toolchain.py --board lsc-smart-indoor
python3 tools/toolchain.py --board lsc-smart-rotatable
```

## 3. Prepare the original firmware files

The build needs files from the original camera firmware. They are kept locally
and are not included in the source download.

Follow [the input setup instructions](docs/INPUTS.md) once for your camera.
If `local/assets/` is already prepared, skip this step.

## 4. Create your signing key

**Already flashed your camera? Reuse its existing signing key.**
For a new installation, create the key for your chosen config:

```sh
python3 tools/keys.py --config lsc-smart-indoor
# Or, for the rotatable camera:
python3 tools/keys.py --config lsc-smart-rotatable
```

Your private key is saved as `local/keys/<config>.pem`; its public key is
`local/keys/<config>.pub`. Existing keys will not be overwritten.
Keep the `.pem` private and backed up: you need it for future updates.

## 5. Compile

### SD card build

Build **all configs**:

```sh
./compile-sd.sh
```

Or build just one:

```sh
./compile-sd.sh --config lsc-smart-rotatable
```

When using a **new key for the indoor camera**, include its public key:

```sh
./compile-sd.sh --config lsc-smart-indoor \
  --trust-public-key local/keys/lsc-smart-indoor.pub
```

SD builds find generated keys in `local/keys/`. To use an existing key elsewhere,
add `--private-key /path/to/your-key.pem` for a single-config build.

### Web update build

Run this and select a config and private-key path when asked:

```sh
./compile-web.sh
```

Or enter them directly:

```sh
./compile-web.sh --config lsc-smart-rotatable \
  --private-key local/keys/lsc-smart-rotatable.pem --sequence 20
```

Web builds compile **one config**, never all of them. Choose a sequence number
higher than the camera's accepted update sequence. For indoor builds using a
new key, also pass `--trust-public-key local/keys/lsc-smart-indoor.pub`.

To see configs or help:

```sh
./compile-sd.sh --list-configs
./compile-web.sh --help
```

## 6. Find your files

Packages are saved in:

```text
output/<config>/candidate/sd.zip
output/<config>/candidate/web.zip
```

These are test candidates. Follow [the update and release instructions](docs/UPDATES.md)
before installing them. Compiling does not flash the camera.

## More information

- [Detailed build options and batch key settings](docs/BUILDING.md)
- [Original firmware inputs, keys and licensing](docs/INPUTS.md)
- [GPIO and board configs](docs/PROFILES.md)
- [Adding camera/CPU support](docs/MODULES.md)
- [Build and test results](docs/VALIDATION.md)
