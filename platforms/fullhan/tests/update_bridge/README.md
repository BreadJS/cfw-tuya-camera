# Update bridge host verification

Run `python3 custom-firmware/tests/update_bridge/test_update_bridge.py` from the project root. Requires a C compiler, Python cryptography, ASan/UBSan, and `nm`.

The twelve tests compile the actual bridge, state machine, authenticated apply planner, signature verifier and JSON escaper. Private disposable fixtures live under `/tmp`; the public RFC8032 example key signs synthetic, nonbootable images. The bridge's `CAMERA_UPDATE_INSTALL_TEST` hooks replace platform eligibility/ownership/RAM checks, fixed helper/progress/MTD-map paths, process/thread failure injection, and sync/reboot. Planner/state ownership, signatures, retained flock and capsule reconstruction remain real. Production builds do not enable those hooks.

A separate metadata-only fake executable runs through real fork, start gate and exec. It checks fixed descriptors 3–7, the retained exclusive lock, readonly trust key, stdio, removed extra descriptors, and dropped inherited environment. It writes only private test receipts and journal fixtures; it contains no flash installer or MTD code. The test executable's undefined-symbol table must contain neither `sync` nor `reboot`. Success tests exercise only the bridge's response to exit 0; they do not establish physical installation or hardware eligibility.

Coverage includes:

- A preexisting descriptor 120 above a lowered soft limit of 96 is closed before exec.
- Fork failure and monitor-thread failure never execute the fake helper; the latter exercises the actual child start gate.
- Stale reviewed sequence or digest prevents process execution.
- Exit 0 holds close at `-EBUSY` during the real two-second grace, invokes only mock sync/reboot, and leaves accepted sequence unchanged.
- Exit 10 permits discard only with the same-process pristine witness and matching signed header; dirty exit 11, an older journal, and mismatching headers block discard. Pristine and dirty retry cases preserve the witness correctly.
- Valid, malformed, injection-shaped, embedded-NUL and oversized progress files remain a bounded JSON string and cannot override authoritative installation phase.

The suite does not open actual camera devices, invoke a real installer, reboot the host, or prove the NOR/JFFS2/watchdog/media-isolation prerequisites. Real boot confirmation and authenticated installed-byte reconstruction have separate suites under `boot_confirm` and `boot_runtime`.
