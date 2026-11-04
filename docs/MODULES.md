# Module interface

`shared/` is maintained once. It contains HTTP/auth, RTSP/H264 metadata handling,
MP4 preview, display settings, credentials, clock, login and crypto. Each
platform's picture adapter supplies its exact schema, units, dependencies and
supported hardware actions; `picture_common.js` supplies the shared field
renderer and `ui.html` supplies the single dashboard/controller.

A SoC file selects `platforms/<backend>/module.json`. The descriptor registers:

- `id`, `schema`, local `adapter.py` filename;
- supported GPIO/feature/PTZ drivers and reviewed flash sizes/partition map;
- runtime configuration/login paths;
- backend host regression entrypoints.

The Python adapter provides `verify(work, profile)`, `application(work, cc,
options)`, `image(work, cc, options)`, `package(work, cc, options)` and
`import_assets(work, source, options)`. Build functions return their artifact
directory. `package` returns a map from `web`/`sd` to filename/bytes, plus
`_images` containing the exact canonical images used for boot qualification.
The root publisher controls release evidence, manifests, candidate names and ZIPs.
Backend functions never contact a device or flash it during compilation.

`layout.json` maps each backend recipe's workspace-relative input to one public
source file. Origins must stay in `shared/` or that backend; cross-backend source
imports and path traversal are rejected. Generated workspace copies allow the
existing reviewed recipes to retain internal relative paths. They are build
products, not two maintained copies of common services. Shared edits affect both
boards, and both regression sets must pass.

To add a board for an existing SoC, copy the nearest profile, choose a new ID and
compatibility identity where needed, specify actual wiring/capabilities and
review sensor/driver/kernel/update assumptions. Validate it using `check`, add
regression tests, compile, then collect exact-image runtime evidence.

To add a SoC, add its descriptor and backend module: a real media provider,
GPIO/board operations, boot/network integration, bounded allocator/ownership
rules, installer/partition constraints, input importer and transport packagers.
Register appropriate feature drivers and profile-header generation for any new
GPIO protocol. Root `build.py` discovers the adapter by its descriptor; it has
no chip-name dispatch switch. Existing header-generation branches implement the
two current IR-cut/LED/PTZ protocols and must be extended for new protocols.

Do not copy the dashboard/auth/streaming stack into the new backend. Add shared
interfaces or picture adapters where hardware differs. A model selection cannot
provide missing ISP/encoder/sensor support by itself. Keep proprietary inputs
ignored, preserve third-party notices, and add host CI plus explicit device
qualification for the new hardware.
