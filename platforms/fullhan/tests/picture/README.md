# Picture settings contract

`src/picture/settings.[ch]` owns defaults, validation, exact form parsing,
flat JSON and private durable storage for the48 advanced picture fields.
It performs no camera, ISP, sensor or encoder I/O.

`camera_picture_valid` returns1/0. Load/save/parse/JSON return0 or a negative
errno; malformed data and insufficient JSON capacity preserve caller output.
Missing picture.conf loads defaults without creating a file. Unsupported
encoder rate modes/nonzero bitrate are rejected rather than silently ignored.
Manual exposure above33000microseconds is rejected for720p30; the inactive
manual value may remain higher while automatic exposure is selected.

## Canonical256-byte format

| Bytes | Content |
| --- | --- |
|0..7 | CAMPICT1 magic/version |
|8..159 |38 little-endian32-bit root fields, in descriptor order |
|160..223 |64-byte independently checked encoder settings record |
|224..251 | Reserved, must be zero |
|252..255 | CRC32 of bytes0..251 |

The first eight root words are the ISP fields. These are followed by exposure,
white balance, gamma, six shadow fields (including shadow_target), CCM mode and
twelve signed CCM values. The embedded encoder record includes its own magic,
version, length, reserved words and CRC. Signed root fields use canonical
two's-complement little-endian encoding. CRC detects corruption, not authenticity.

The directory must be private and owned by the service UID. picture.conf must
be a single-link regular0600file with the same owner. Links, public modes,
size changes and changed metadata during reads are rejected. Saving writes
picture.tmp.PID.N exclusively, syncs it, atomically renames it and syncs the
directory. A failure after rename can already be committed; callers re-read
the actual state on save failure. Existing settings.conf remains untouched.
The factory-reset owner must remove picture.conf and its known temporary names.

The form accepts exactly48 known keys, with numeric0/1 booleans. It rejects
duplicates, missing/unknown/extra fields, malformed encoding/numbers, overflow
and unsupported cross-field combinations. JSON uses identical flat names;
the web application supplies its outer status/capabilities wrapper.

## Validation

`python3 tests/picture/test_settings.py` runs ASan/UBSan checks covering:

- Default JSON/form and private-file round trips.
- Signed CCM/luma/hue extremes and supported720p30 manual exposure boundary.
- Every major field bound, malformed/overflow syntax and missing/unknown/
  duplicate/extra form keys, with immutable output on failure.
- Exact canonical offsets and both checksummed record layers.
- Corrupt magic/data, reserved-byte violations with recomputed outer CRC,
  invalid semantic data with valid CRC, truncated/oversized files.
- Atomic inode replacement with an old open descriptor remaining unchanged.
- Public modes, symlinks, hardlinks and nonprivate directories.

Strict static ARMv6 soft-float compilation also passed. These are host/storage
tests; applying picture controls to hardware remains the pipeline owner's job.
