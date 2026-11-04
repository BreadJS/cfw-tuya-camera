# Client admission failure isolation

`camera_rtsp.c` now closes only the new client when TCP_NODELAY, CSPRNG or TX
allocation fails. No fallback randomness is introduced. Accept interruptions,
pending network errors and resource errors retain the listener and existing
clients. Only invalid listening-descriptor errorsEBADF/ENOTSOCK/EINVAL/
EOPNOTSUPP remain fatal. ListenerPOLLERR/HUP/NVAL are fatal explicitly.

Recoverable admission failures disable polling the listener for100ms. The
normal5ms event loop continues handling existing clients and calling capture.
The disabled listener haspollfd.fd=-1, so persistent readable backlog under
EMFILE cannot create a retry spin.

`python3 tests/test_client_failures.py` runs the production event loop on
loopback with14 individual faults injected at actual admission calls. Each
case verifies existing authenticated RTSP+RTP continues, a later client can
connect with a distinct session, and the provider continues advancing. A
separate sustainedEMFILE test leaves the connection pending in the real
kernel backlog:3 failures take at least250ms, exactly5 admission attempts
occur including2 successes, and media continues. All fixtures stop afterward.
Synthetic tiny NALs in this focused test prove transport liveness, not decode.

Existing integration.py also PASS: mainH264+PCMA and subH264+L16, FFprobe,
FFmpeg A/V decode, disconnect/reconnect and stalled-client isolation.
Sender-report regression and strict staticARM build pass.

This does not establish why the observed detail-profile trial lost its
listener. A provider error or another process-wide failure remains possible.

## Minimum future runtime telemetry (proposal; no API changes here)

Expose one coherent, locked snapshot through the existing status backend:

- RTSP running/listening, start/exit monotonic timestamps, exit category
  (stop/setup/poll/listener/credentials/video-provider/audio-provider/invalid
  frame), saved errno and original provider return value.
- Video/audio callback count and last successful frame timestamp/byte size,
  plus source-error/invalid-frame counts. Save errno immediately when the
  callback returns; cleanup and subsequent libc calls can overwrite it.
- Current/peak clients; admission failures groupedaccept/socket-option/
  randomness/allocation; last error and admission-backoff count.

Existing credentials-token failure currently breaks the run loop with its
result still0; exit-category telemetry must distinguish it from normal stop.
Counters should not include credentials/session secrets. Do not infer capture
health solely from AE/WB activity or the existence of an HTTP server.
