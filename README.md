# TTGK Audio Bridge for PS5 (experimental)

Fork of [ps5-solobridge](https://github.com/hebiii994/ps5-solobridge), GPL-3.0. Target: TTGK Audio 3302:33c0, UAC2 high-speed USB, synchronous stereo 48 kHz / packed 24-bit PCM. The resident bridge no longer targets Scarlett Solo; upstream diagnostic utilities are retained.

**Not yet tested on a PS5.** Upstream capture worked on 13.60; 13.00 is the intended test platform, not a confirmed compatibility claim. HDMI still carries sound. Tempest and the PS5 headphone slider are not integrated.

## Controls

Hold L1 + R3 alone for at least 500 ms. While both remain held:

- Up / Down: +2 / -2 dB (repeat approximately every 150–190 ms, sampled at 25 Hz)
- Left / Right: hardware mute / unmute
- Release either modifier to disarm.

Direct read-only DualSense API is experimental and may be denied to a resident payload, especially during a game. No system process injection, privilege changes, controller emulation or input suppression. The game may receive the same buttons. The payload logs controller API failures and continues audio. Notifications are rate-limited.

## Local fallback (no listener)

No HTTP or TCP server. A single-instance file lock prevents duplicate playback. Default volume -40 dB; startup verifies hardware mute, writes and reads back both volume channels, then applies the desired mute state. **Keep headphones off your ears on the first test** and verify volume independently.

- /data/solobridge/volume: verified persisted volume state. Integer 0..148 maps to -74..0 dB in 0.5 dB steps. Add 256 to request mute. Example: 68 = -40 dB, 108 = -20 dB.
- /data/solobridge/volume.request: write one integer plus newline to a temporary file and rename it to this name atomically. Consumed within about one second. Works if controller API is denied; use an existing local file-management/FTP payload, if available.
- /data/solobridge/stop: create an empty file to stop gracefully. SIGINT/SIGTERM also stop the resident bridge.
- /data/solobridge/log.txt: startup and periodic playback diagnostics; diagnostics.log: USB/controller failures.
- /data/solobridge/disable_pad: create before launch to disable experimental controller calls while testing audio and file controls. Remove before next launch to enable.

Do not use the upstream solobridge.ps1 TCP status/stop script with this fork.

## Build and first test

With the upstream Docker build environment (Ubuntu 24.04, PS5 Payload SDK):

    docker build -t ps5sdk .
    .\\build.ps1 solobridge

Or make -C solobridge PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk.
Send solobridge/solobridge.elf to an already-running ELF loader (9021). Do not enable autoload until manual testing succeeds. Upstream installer/autoload remain optional; rebuild installer after the new ELF is built.

Host-independent tests:

    cc -std=c11 -Wall -Wextra -Werror -O2 tests/test_logic.c -o /tmp/ttgk-tests
    /tmp/ttgk-tests

Test matrix on firmware 13.00:
1. Start without DAC; verify hotplug.
2. Connect TTGK at high speed. Check sample-rate readback, both channels, quiet default and no clipping.
3. Test hardware volume using volume.request before testing DualSense.
4. Test game/menu capture, transitions, pad reconnect, sign-out/in and L1+R3 chords in a game; note conflicts.
5. Check packet underruns, delays and hardware control latency. Control ioctls can block and must be measured on hardware.
6. Disconnect/reconnect DAC, verify persisted volume, stop and restart. Only then enable autoload.

Known TTGK USB topology: AudioControl interface 0, playback interface 1 alt 2, Clock Source 9, Feature Unit 2 (master mute; per-channel volume), endpoint 0x01 with bInterval=1 and 288-byte max packet at high speed. The bridge validates the observed layout and sends 36-byte microframes. Full-speed and unrelated devices are rejected. The DAC HID report descriptor and physical buttons have not been verified.

Upstream ring-buffer/drift logic retained, with bounded capture/USB work, reset on reconnect/session loss, corrected full-scale PCM conversion and removal of blocking TCP listener. No software gain adjustment.

Controller ABI references: https://github.com/sub0x00001/octet and https://github.com/blackbearreloaded/ps5-native-gamepad-input-research.

GPL-3.0; see LICENSE and solobridge/NOTICE.md.
