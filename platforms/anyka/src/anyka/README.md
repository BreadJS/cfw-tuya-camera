# Anyka native backend

The exact original SDK now supports native MIS2006 H264 video and microphone
audio through the portable media-provider contract. `sdk_abi.h` and `sdk_api.c`
contain the reviewed ABI and exact-version bindings. `media_backend.c` owns
capture/encoder initialization; `audio_backend.c` copies and releases PCM leases.

`rtsp_trial.c` is an experimental SD/RAM entry point. It requires exclusive media
ownership and the validated patched SDK host. It does not install firmware.
See ../../docs/ANYKA-MEDIA-TRIAL.md and ../../docs/ANYKA-AUDIO.md.

The owned application also integrates bounded PTZ movement and homing, physical
day/night and IR controls, the reset button, Wi-Fi/recovery and the APP updater.
See ../../docs/STATUS.md for measured hardware results and remaining installation
checks. RAM testing does not establish a successful NOR installation.
