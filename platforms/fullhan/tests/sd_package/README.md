# SD package host tests

Run `python3 custom-firmware/tests/sd_package/test_sd_package.py`.

Seven groups test the exact source identity and ARM instruction anchors, all256
CRC32 table entries, complete partition layout, canonical image FF padding,
header/CRC/length corruption, protected/out-of-range/duplicate/reordered records,
non-FF tails, invalid embedded images, CLI exclusive creation and inspection.
An8MiB bytearray model applies only changed64KiB blocks: all114 initial blocks
change, bootstrap/env/U-Boot/config remain identical, a second application changes
zero blocks, and an interrupted7-block application plus107-block retry converges.
The model is not an emulator or physical readback test. No device is contacted.

API: `sd_package.build({2: kernel_bytes,3: rootfs_bytes})` returns bytes;
`sd_package.inspect(data)` returns JSON-ready metadata. Import the tools directory
on `sys.path` so the existing `firmware_package.check_image` module is available.

CLI:

```sh
python3 custom-firmware/tools/sd_package.py --kernel kernel.uImage --rootfs rootfs.squashfs --output /some/new/output/path
python3 custom-firmware/tools/sd_package.py --inspect /some/new/output/path
```

Output creation is exclusive and never overwrites an existing file. The builder
does not copy to SD, mount anything, invoke the updater or reboot. The SD updater
format differs from signed web OTA and daemon packages. Exact saved evidence and
limitations are in `docs/evidence/sd-linux-bin-format-evidence.md`.
