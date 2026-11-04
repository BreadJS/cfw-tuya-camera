# Browser preview transport

Original bounded C H264-to-fragmented-MP4 muxer and HTTP stream hub. It copies
already encoded video and never decodes, re-encodes, opens capture devices or
changes the sensor, ISP, encoder, RTSP or microphone configuration. Browser
preview is video-only; existing RTSP audio is independent.

`preview.h` is the integration boundary. Create a hub from actual encoded
dimensions and `camera_media_description`'s SPS/PPS NALs (without start codes).
After each successful native video-provider callback, publish its complete
Annex-B AU using its original `pts_us` and `keyframe`. Provider memory is copied
synchronously before returning. Preview failures must not stop RTSP/capture.
Call `preview_stream(hub, fd)` only after the main HTTP route authenticates its
GET request. Success transfers descriptor ownership to a detached stream
worker; an error leaves ownership with the HTTP service. `preview_mime()` is
suitable for the browser's `MediaSource.isTypeSupported/addSourceBuffer` and
the main app's authenticated preview-status JSON.

The input contract is H264 Baseline/Main with no B/reordered pictures: PTS
equals DTS. Current camera video meets that contract. One pending AU allows
duration to come from the next source timestamp, adding approximately one
frame of mux latency. Fragments use 90kHz timescale, positive variable sample
durations,64-bit `tfdt`, movie-fragment-relative `trun` offsets and correct
key/non-key sample flags. Initialization contains `ftyp`, empty sample tables,
`avcC` and `mvex/trex`. SPS/PPS stay out-of-band for `avc1`. A configuration
change requires a new hub/initialization segment; changed in-band parameter
sets reject preview output rather than silently misdescribing the codec.

Source publication never waits on a mutex or network. It uses trylock and a
fixed four-slot reference-counted ring, with at most two readers. There are
four fragment slots of512KiB plus1KiB overhead, one512KiB pending AU, small
configuration metadata, and two64KiB worker stacks: approximately2.7MiB total,
excluding kernel socket buffers and the capture backend. Storage is allocated
once on hub creation; no per-frame allocation occurs. Inactive hubs do not
copy/convert source video. Stream thread creation/configuration happens outside
the media mutex.

Readers pin a slot only while sending it. An individual send is limited to1s
without progress or3s total; a source/IDR wait is limited to5s. New readers
start at a future IDR. If a reader falls behind the four-slot ring, or a frame
is lost to contention/timestamp invalidity, its reference chain is discarded
and its connection closes for browser reconnection. The producer continues
without waiting for that reader. There is no unbounded per-client queue.

The browser receives an HTTP200 close-delimited `video/mp4` body: one init
segment followed by `moof/mdat` pairs. The first IDR of a late viewer can have
nonzero decode time because all clients share the source timeline. The page
must seek to its first buffered range/live edge, serialize append operations,
trim old buffered video, and recreate the MediaSource on disconnect/settings
changes. Authentication, CORS/origin policy, the dashboard player and auto
reconnect belong to the main control application, not this hub.

## Offline validation and fixture

```
make
make arm
make test
SANITIZE=1 python3 tests/test_preview.py
```

Tests generate synthetic FFmpeg H264, compare all30 decoded frame hashes
against both constant/variable-duration MP4 at320x180 and1920x1080, verify
box/sample timing and ffprobe codec/resolution/count, then exercise live
loopback late join/reconnect and a slow TCP reader alongside a decoding client.
Core tests cover the two-client cap, bounds, changed SPS, source timestamp
regression, and a held mutex that must not block source publication.
All temporary test servers terminate in `finally`.

For an explicitly retained host-only browser QA session:

```
build/fixture /tmp/generated-with-aud.h264 1920 1080 18990
```

The fixture binds **127.0.0.1 only**, loops until SIGINT/SIGTERM, and serves
`/api/preview.mp4`, `/api/preview/status`, and a minimal MSE test page at `/`.
It deliberately has no authentication and must not be shipped or exposed on a
LAN. It reads an entire synthetic file (up to16MiB) only for host testing; the
production hub does not load recorded files. Required FFmpeg parameters include
`aud=1:repeat-headers=1:keyint=15:bframes=0`; encode Baseline/Main only.
An optional fifth filename argument writes offline MP4 instead of starting a
listener; a sixth argument chooses variable fixture durations.

Protocol reference: [W3C ISO BMFF byte-stream format for Media Source
Extensions](https://www.w3.org/TR/mse-byte-stream-format-isobmff/), particularly
initialization segments, movie-fragment-relative addressing, decode time and
random-access points. No third-party mux implementation is vendored.

Host protocol/decode validation is separate from target memory/CPU acceptance,
actual camera browser playback and the main application's reconnect behavior.
