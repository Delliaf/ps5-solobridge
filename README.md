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

No HTTP or TCP server. A single-instance file lock prevents duplicate playback. Default volume -40 dB; startup verifies hardware mute and both volume channels; unmute is deferred until the stream is ready. **Keep headphones off your ears on the first test** and verify volume independently.

- /data/solobridge/volume: verified persisted volume state. Integer 0..148 maps to -74..0 dB in 0.5 dB steps. Add 256 to request mute. Example: 68 = -40 dB, 108 = -20 dB.
- /data/solobridge/volume.request: write one integer plus newline to a temporary file and rename it to this name atomically. Consumed within about 250–350 ms when the file worker is available. Works if controller API is denied; use an existing local file-management/FTP payload, if available.
- /data/solobridge/stop: create an empty file to stop gracefully. SIGINT/SIGTERM also stop the resident bridge.
- /data/solobridge/log.txt: startup, USB/controller errors and periodic playback diagnostics (rotated).
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

## v2 audio engine

Capture IPC, USB control, input/persistence and bounded log output run separately from the isochronous writer. A lock-free single-producer/single-consumer ring, bounded queue pacing and fractional-phase interpolation replace abrupt periodic sample drops/duplicates. Short ramps only smooth discontinuities; volume is still exclusively hardware-controlled. Hardware mute is held until the stream is prebuffered and packets have been accepted. Clock validity is checked. USB control sends only changed values and verifies readback; failures close output and require a muted reconnect. No USB transfers are attempted through a closed control descriptor. The writer does not wait on USB control during normal operation; teardown may wait for a kernel ioctl to return.

The default maximum volume is -6 dB. Set /data/solobridge/max_volume to one integer (0..148, no mute flag) before launching; 148 enables 0 dB. Saved values are clamped. Commands are ignored while disconnected. Logs rotate at 512 KiB with one previous file; errors are now recorded in log.txt rather than diagnostics.log. The USB queue depth is an estimate, not measured hardware latency. A persistent write blockage or large timing gap triggers reconnection. Capture sessions without data for 2 seconds are restarted. USB_FS batching and explicit system rest-mode events are not implemented; both need PS5 testing.

Host tests also exercise ring overflow, prebuffer, underrun, PCM packet packing, phase correction and pacing. The console ABI, actual controller access, USB control latency, and measured end-to-end delay remain unverified.

The hardware transaction tests use mocked verified USB operations to check mute-before-level, no redundant requests, stale-unmute prevention and partial channel failures. A 500 ms control watchdog closes the streaming endpoint on a slow transaction; teardown cannot forcibly cancel undocumented or blocked kernel calls. File commands have a separate worker from DualSense; shutdown may wait for a stuck system API after audio has been stopped.

Volume requests normally wake the hardware worker using a POSIX semaphore (timed/polling fallback); no periodic USB control traffic is generated when volume and playback state are unchanged. Capture generations prevent a late start from an old USB session being treated as current. Host tests include concurrent ring wrap-around and simulated ±500 ppm drift over 60 seconds; these do not substitute for USB hardware measurements.

## TV Tempest and native DualSense room measurement (experimental verification)

See [TV_TEMPEST.md](docs/TV_TEMPEST.md) for the actual native setup and opt-in A/B and calibration signal capture. This is diagnostic support only: the source is still AVCAP Remote Play, and native TV Tempest/measurement propagation through it is not confirmed. No microphone capture, fake surround, kernel patches or additional network services. Disabled by default.

## Local web diagnostic panel (recovery build)

No access key, no dependency on /dev/random, no startup notification call. Open http://PS5-IP:9160/ using the numeric private IPv4 address. If 9160 is occupied, try 9161. The server opens and retries in its own worker, independent of DAC/capture initialization. The actual stage, port and errno are written to /data/solobridge/web_status.txt, stderr and log.txt. GET /health responds TTGK Web OK. A persistent disable_web file still disables web at startup; remove it and relaunch to re-enable. The stale web_key file from older versions is ignored.

No authentication: any host on a permitted LAN can control volume, read logs and stop the payload. Do not expose the port beyond a trusted local network. Only numeric private IPv4 Host headers are accepted; browser POST actions use a non-secret X-TTGK-Action: 1 header and same-origin check. These are CSRF/DNS-rebinding mitigations, not access control. No CORS. Audio and existing hardware volume limits are unchanged.

Live hardware readback, controller status/raw button mask, bounded 60-second pad trace, audio counters, and log downloads remain available. Disable web until restart, persist disable, or stop the whole payload. Volume notifications are off by default because native notification calls are not validated on 13.00. To experiment, create enable_notifications before launch; failures are logged. No notification is needed for web access.

Host tests exercise the actual HTTP socket server, no-key access, foreign Host/Origin rejection, volume, logs, slow client, fallback port and both shutdown paths with mocked hardware. PS5 socket behavior and native controller API still require hardware verification.

## v2.1.0 — lightweight web-only control

Removes DualSense input worker, native pad module loading and button logging. Calibration research is unchanged. The listener waits in select with a shutdown socketpair (1s timeout fallback), status snapshots are requested only by HTTP, logs sleep on a condition variable, and USB volume worker safety wake is 1s rather than 100ms. Browser polling stops when hidden, status interval 2s, log auto-refresh off by default. Local stop/volume files remain supported; file poll is 1s. Startup notification and manual test use a separate event-driven worker; volume notifications still opt-in via enable_notifications. Full notification struct with icon and target -1, zero-header fallback only on API failure, return code logged; actual on-screen rendering requires PS5 test. Version is centralized in version.h, embedded in status/health/log/notification/version.txt and artifact filename. No changes to the verified PCM streaming, USB hardware commands or calibration.

The actual control worker is host-tested with a mocked notification API (full ABI, version string, fallback, result reporting, and persistence). Browser JS syntax and shutdown of an idle HTTP listener are also tested. The local file worker wakes once per second; its persistence debounce may take up to about two seconds.

## v2.2.0 — measured pacing optimization

The default economy scheduler refills approximately eight packet credits at a time (up to 1ms sleep), retaining the verified 36-byte per-write USB format and 48-packet estimated upper queue limit. Legacy scheduling is selectable in web UI or by creating legacy_pacing before launch. The web switch is temporary. No packed multi-packet write: upstream FreeBSD ugen isoc callback consumes FIFO entries as individual frames, so concatenation may change packet boundaries. CPU process user/system time from getrusage and actual write attempts/main loops/sleeps per second are sampled every 5s and logged every 30s. CPU percentages are relative to one core, not total console load, and exclude work charged to other processes or asynchronous drivers. Hardware test required; do not assume FPS improvement. No calibration changes.

## v2.3.0 experimental USB_FS benchmark

Normal startup remains the tested economy write backend. Explicit web benchmark tests baseline, 16, 32, 64 microframes per USB_FS transfer, with three outstanding slots, 5s warmup and 25s measurement each. Each frame remains 36 bytes, 48k stereo packed 24-bit. Uses FreeBSD 11 USB_FS ioctl ABI (not verified on PS5 13.00), validates actual frame count/length and completion status. Errors/timeouts end a phase; normal mode is restored after completion/cancel. Do not start during important gameplay or downloads; a firmware-specific driver hang cannot be ruled out. Download text report and full log. PASS_COUNTERS is not proof of audible sound. CPU is process time relative to one core. No automatic permanent selection; no calibration changes.

Host tests mock the USB_FS kernel API and verify frame arrays, short/status errors, partial-open cleanup, benchmark acceptance criteria and HTTP start/cancel/report. They cannot establish actual PS5 support, timing, audible output or kernel responsiveness. A failed driver call may require a console restart. Results are in memory until downloaded (also written to bounded log); do not stop the payload before downloading.

## v2.4.0 latency research

Normal startup unchanged. Benchmark compares eleven explicit profiles (see bench.h): baseline and confirmed 64x3, 32/24/16/8 x2, reduced AVCAP read requests, faster and predicted polling. Smaller read capacity does NOT prove smaller producer blocks; measured record sizes, inter-read intervals and queue samples are reported. USB_FS uses one completion ioctl per readiness event in optimized profiles instead of an extra empty ioctl. Low-latency profiles trim only muted startup preroll, lower reserve, never drop audio intentionally during steady state (existing drift and emergency overflow recovery remain). PCM ring producer uses at most two memcpy calls; same float data and conversion. No guessed blocking flags or undocumented kernel patches. Queue statistics are sampled estimates (ring plus upper-bound in-flight packets), not measured A/V latency. Reduced AVCAP requests may fail; the benchmark times out and restores normal mode. Experimental phases are not persistent. Confirm audio by ear and measure HDMI vs DAC externally. Calibration unchanged.

Additional v2.4.0 validation: 12 profiles including identical 16x2 scalar vs SSE2 conversion. SSE2 output and batch consumer tested byte-for-byte against scalar; one read cursor publish per USB block, no fast-math. Per-thread CPU measured if CLOCK_THREAD_CPUTIME_ID is supported (source sampled at 250ms; approximate). Native driver query uses only read-only USB_GET_IFACE_DRIVER; no detaching, patching or registration. No confirmed smaller producer grain or safe AVCAP blocking ABI. Preroll discard separately counted; only while starting/restarting, not hidden during normal playback.

See [docs/LATENCY.md](docs/LATENCY.md) for all investigated optimizations, evidence limits and physical A/B measurement using `tools/measure_latency.py`. Benchmark now also includes sampled queue p95 and validates source throughput. Physical measurement is required before claiming a sub-frame latency.

## v2.5.0 measured profiles

Based on PS5 13.00 results: exclude rejected sub-1024 AVCAP reads and under-rate USB_FS x8. Compare safe write, previously successful adaptive x16x2 scalar, x16/24/32 with 2/3 slots and adaptive+SSE2, and a 48-frame reserve candidate. One round or two in opposite order, 8s warmup and 45s measurement. Queue and ring statistics separate; report empty readiness and select timeouts. Manual profile selection lasts until payload restart; default safe write. Runtime guard returns to write after setup/stream error or repeated bad rate windows, recurrent underruns or detected data loss. No automatic selection based on estimated latency. Native driver active ioctl is read-only. Calibration unchanged.

Ten profiles in final v2.5.0 (quick 1 round, full 2 rounds): profile 9 tests a 19ms cap for predictive AVCAP sleep with 2ms guard. New combined candidates use 240-frame muted prime reserve, then existing bounded drift correction toward 96 (or 48) floor. Previous adaptive x16x2 scalar is retained as control. Reports distinguish clean steady-state from warmup underruns (PASS_STEADY_WARN_STARTUP), actual reserve and first/last 5s queue growth; >4ms growth or tighter packet-rate mismatch fails. Runtime telemetry includes FS calls, packets, empty wakes. The startup guard and loss guard are conservative, not an assurance of zero audible glitches. No actual 60 FPS subframe latency or 0.5% CPU proven.

## v2.6.0 focused completion-queue experiment

Based on the full two-round v2.5.0 hardware report (10 profiles). Profiles 9 and 6 passed both startups and steady measurement. Each USB_FS completion produced one additional empty readiness event. FreeBSD 11 `ugen_fs_get_complete` clears `flag_iscomplete` only when dequeue finds no event; v2.6 candidates drain through the empty completion BEFORE resubmitting, at most slots+1 completion ioctls. No new kernel ABI or packet-size change. Profiles 10 and 11 are controlled comparisons with 9 and 6; 12–14 explore 24 frames and two slots with 336-frame startup reserve (only at startup). Benchmark only tests 9,6,10–14, in reverse order on second round. Old profile numbers retained; no default change. Verify actual PS5 improvement and listening quality; queue is not measured A/V delay. Calibration unchanged.

## v2.6.1 short benchmark

Same audio engine and selectable playback profiles as v2.6.0. Both benchmark start endpoints now run exactly one round of profiles 9 (known control), 10 (matched drain experiment), 13 (two slots, larger startup reserve). 8 seconds of stable warmup plus 30 seconds measured per profile, about two minutes when healthy. Startup warnings, queue, throughput and CPU checks retained. No profile is automatically selected. Other profiles are still available manually. Short screening does not replace prolonged gameplay testing.
