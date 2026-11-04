# Unified camera firmware — implementation ledger

User objective: one GitHub-ready project supporting LSC Smart Indoor IP Camera
(Fullhan FH8626V100/MIS2006) and LSC Smart Rotatable Camera
(Anyka AK3918EN080 V300S/AK39EV330/MIS2006), selectable CPU/board/features,
configurable GPIOs, real backend modules and shared maintained code.

## Requirements and completion evidence

- [x] One standalone source root; neither build imports either legacy project.
- [x] Shared HTTP/auth/RTSP/preview/settings/clock/Telnet implementation; reconcile
      divergent fixes, do not hide two complete source copies behind a wrapper.
- [x] Shared web layout/controller with platform picture/control adapters;
      capabilities govern which controls are visible/usable.
- [x] Board JSON profiles name product, SoC/toolchain/sensor, GPIO controller,
      pin polarity, IR-cut mode/timing, LEDs, reset, PTZ driver/limits and features.
- [x] Profile values generate actual C/script build inputs and affect GPIO
      operations. Tests change pins/polarity/capabilities and verify emitted/use
      paths, reject invalid/incompatible profiles and pin collisions.
- [x] CPU backend modules retain exact media ABI/hardware/flash boundaries;
      board changes do not pretend a GPIO list can implement a new SoC driver.
- [x] One CLI and web/SD wrappers list/select boards, check inputs, build
      applications/complete images and package correct transport per platform.
- [x] Separate output by board; signed update identities/keys/partition pins
      remain isolated. Do not reuse old boot evidence for changed image bytes.
- [x] Import local vendor assets/toolchains/keys explicitly; keep ignored and
      exclude personal data, binaries, original dumps and keys from source ZIP.
- [x] Both actual ARM builds and complete images verified; shared regressions
      and platform-relevant host tests pass. No device flashing is requested.
- [x] Source-only export/fresh-checkout build works with explicit local inputs,
      without investigation folders or either legacy directory.
- [x] README documents quick start, profiles, module API, adding boards/SoCs,
      prerequisites/licensing, GPIO limits, candidate/boot qualification and
      SD/web distinctions. CI checks public source/configs/shared host code.

## Architecture being implemented

`shared/` holds one maintained common implementation. `platforms/<backend>/`
holds SoC media/hardware/boot/update modules and their specific build recipes.
`boards/` describes wiring/products; `soc/` describes the ABI/module catalog.
A generated build workspace under ignored `output/` composes those modules,
allowing the reviewed firmware packagers to keep their internal paths while
removing maintained-source duplication. Generated copies are not source forks.
Profile headers/configs are compiled into each target and included in manifests.
No symlinks are required on this network-share workspace.

## Completion evidence

The original two projects remain untouched. Both board profiles compile real ARM
applications, complete images and both transport packages. Shared source is
maintained once; the common dashboard uses platform picture adapters and one
field renderer. Compiler-included profile headers drive actual GPIO, polarity,
IR-cut timing, motor devices/limits/inversion, reset and service ports. Capability
flags govern hardware initialization, API availability and dashboard controls.

The Anyka build reads its imported DTB to verify kernel-owned LED/key/motor pins
and polarity. Profile validation rejects collisions, unsupported driver fields
and changes to reviewed flash boundaries. A generic JSON pin list cannot replace
a missing chip/sensor backend or rewire an immutable kernel driver.

Restored/pinned Fullhan musl and Anyka uClibc toolchains both cross-build. A fresh
source ZIP in a separate directory builds both targets with explicitly imported
local inputs and no earlier project source. Fresh Anyka AP tools also build.
Host regressions and source/export/qualification tests pass. Private inputs,
original firmware, toolchains and keys are excluded from the public export.

See VALIDATION.md for actual build/test evidence and remaining hardware limits.
No new firmware was flashed. The new source composition is version 0.2.0; final
installer names require exact-image runtime qualification through the documented
release command. The public source folder/archive are ready for GitHub; no remote
repository was published by this task.
