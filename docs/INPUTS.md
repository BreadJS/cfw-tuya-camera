# Local inputs and licensing

The source export intentionally contains no vendor firmware, SDK host, kernel,
kernel modules, cross compiler or private keys. These remain in ignored `local/`
or the user's external cache. Builds reject unknown original-image revisions.
Matching the marketing name or CPU alone is insufficient.

For an existing prepared project, `tools/import_local.py` copies its `base/` and
Anyka network cache once. Afterward, builds read only this project's ignored local
inputs. The older projects can be absent. Import receipts include hashes; no
factory Wi-Fi configuration or accounts are copied into newly built firmware.

For a fresh clone, prepare these owner-provided inputs:

| Board | Files in `--input-dir` |
| --- | --- |
| Indoor | `kernel.uImage` (exact stock image), `rootfs.squashfs` (stock filesystem) |
| Rotatable | `rootfs.bin`, `app.bin`, `kernel.bin`, `dtb.bin` (exact stock partition dumps), `anyka_ipc` (original executable) |

```sh
python3 tools/assets.py --board lsc-smart-indoor --input-dir /private/indoor-inputs
python3 tools/assets.py --board lsc-smart-rotatable --input-dir /private/ptz-inputs
```

The Fullhan importer extracts only pinned runtime files, validates them against
`platforms/fullhan/config/base-inputs.json`, and applies the exact reviewed
watchdog-init kernel patch with hash verification. The Anyka importer selects
pinned libraries/modules/WPA tools/ISP profile/font, verifies immutable partition
hashes, and prepares the guarded SDK host by blocking original `main`.
These transformations run offline and do not execute the original application.

The Anyka AP tools additionally require the sources pinned in
`platforms/anyka/config/network-sources.json`: hostapd 2.12 archive and a clean
libnl-tiny checkout at the specified commit/tree hash. Build them once:

```sh
python3 tools/network.py --board lsc-smart-rotatable \
  --hostapd-source /private/hostapd-2.12.tar.gz \
  --libnl-source /private/libnl-tiny
```

Neither import path replaces an existing local input cache. Keep a separate
backup before deliberately preparing a new revision. A different revision must
be reviewed and added as a backend/profile port, rather than bypassing hashes.

Keys are separate from vendor inputs. Keep existing camera signing keys for
updating installed cameras. To explicitly create keys for a **new installation**:

```sh
python3 tools/keys.py --board lsc-smart-indoor
python3 tools/keys.py --board lsc-smart-rotatable
```

The command refuses replacement and creates a mode 0600 private PEM plus a raw
public `.pub` file in ignored `local/keys/`. Anyka embeds the selected signing
key's public half in its installer. For a fresh Fullhan bootstrap, pass
`--trust-public-key local/keys/lsc-smart-indoor.pub` together with the matching
`--private-key` to a candidate/image build. Existing Fullhan cameras still check
the trust anchor already installed on them: a different key cannot authorize its
own replacement through a normal web update. Losing a private key loses the
ability to sign further updates accepted by that installed trust anchor.

Project-owned implementation is MIT licensed. Monocypher retains its
BSD-2-Clause/CC0 notices in source; the reference project's MIT notice is in
`docs/third-party/ipcd-LICENSE.txt`. ABI declarations are documented in
`docs/third-party/ANYKA-ABI.md`. Third-party copyright/license notices remain
intact and are not replaced by the project license.

Linux, BusyBox, WPA/AP utilities and the external toolchain have their own
licenses and source/redistribution obligations. The Anyka SDK host and vendor
modules may have additional restrictions. Owning an input image is not blanket
permission to redistribute its proprietary components. The source ZIP excludes
those components; distributing compiled firmware requires checking the licenses
of the actual retained inputs and providing corresponding sources where required.
