# airgroup-pico (experimental)

A Raspberry Pi **Pico W** firmware that plays audio on a Google Cast speaker group. This is
**step 1 of 3** of a port of airgroup to the Pico. The main project uses a Pi Zero 2 W and is
what you should use for music.

| Step | Status |
|---|---|
| 1. Join Wi-Fi, find the group via mDNS, open the Cast v2 TLS session, stream live audio over HTTP | ✅ Works. 90 s test on a 4-speaker group with no dropouts |
| 2. AirPlay 1 receiver: RTSP, RSA/AES, ALAC decode | Not started |
| 3. Connect the AirPlay receiver to the stream; volume sync; reconnects | Not started |

## What it does now

On `GET /test` (or `/test?s=<seconds>`) it finds `CAST_TARGET` and makes the group play a
soft double-beep test tone. The tone is streamed from the Pico as **mono** 16-bit 44.1 kHz live
WAV (`/stream.wav`). Mono is fine because Nest/Home speakers are single-driver, and it halves
the bytes, which matters in 264 KB of RAM.

Other endpoints on port 8090: `/log` (recent log lines), `/bench` (TCP throughput test), `/`.

## Build and flash

Needs the Pico SDK (tested with 2.3.1), the Arm GNU toolchain, CMake, Ninja and picotool.

```bash
cp config.local.cmake.example config.local.cmake   # add Wi-Fi (2.4 GHz) and group name; git-ignored
export PICO_SDK_PATH=~/pico/pico-sdk PATH=~/pico/toolchain/bin:$PATH
cmake -S . -B build -G Ninja -DPICO_BOARD=pico_w   # or pico2_w
ninja -C build
picotool load -f -x build/airgroup.uf2             # board connected over USB
```

The Wi-Fi password goes into a generated header in `build/`, never onto compiler command lines.

## Lessons from step 1

- **Send one segment per `tcp_write`.** Writing tens of KB in a single call often failed with
  `ERR_MEM`, and the stream ran about 7% slower than real time. Writing one MSS at a time fixed it.
- The TLS handshake to a Cast group takes about 0.6 s on the RP2040. mbedTLS peaks at about 32 KB of heap.
- Static RAM use is about 188 KB. That leaves little room for step 2, and a Pico 2 W (520 KB)
  would make the AirPlay receiver much more practical.
