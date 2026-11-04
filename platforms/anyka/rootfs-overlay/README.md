# Runtime overlay

The preserved stock ROOTFS calls `/usr/sbin/main.sh`. Our APP replaces that
entry, loads only the reviewed board's modules and stages its service directory
in `/tmp/camera-runtime` before executing the guarded SDK host with our preload.
It never invokes the original `service.sh`, `daemon` or saved CONFIG scripts.

The network supervisor, WPA tools, hostapd, Telnet login, installer, font, ISP
profile and application all run from RAM. Immutable ROOTFS `/lib` dependencies
remain mounted. The installer separately requires a normal `/usr` unmount;
copying files to RAM alone is not treated as proof that flashing is safe.

The implementation is an offline test candidate, not hardware-validated boot
firmware. See `docs/APP-BOOT.md` for inputs and remaining checks. Do not copy
Fullhan GPIO or watchdog handling into this target.
