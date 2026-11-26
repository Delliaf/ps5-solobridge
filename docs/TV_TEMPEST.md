# Native Tempest 3D Audio for TV Speakers — verification on PS5 13.00

Status: **not confirmed through AVCAP**. The bridge does not change Sony's audio policy, create a virtual headset, patch kernel memory, implement fake surround, or run its own microphone calibration. It preserves the native stereo capture. This diagnostic is opt-in and does not add a web/TCP listener.

## Why this test is required

Sony's TV 3D output is rendered to stereo and can use acoustic measurements made with the controller microphone. The current payload opens libSceAvcap2 source 2 (Remote Play mix), not HDMI endpoint PCM. There is no verified public post-TV-Tempest capture API for firmware 13.00. Sony's 2024 Remote Play limitation is not timeless proof: later PS Portal software advertises 3D audio on supported devices, but does not document that AVCAP source 2 carries the TV-specific room-calibrated stereo render. The FGG-PlayPods issue #5 documents a different case (headphone rendering on firmware 12.40): the per-device output changed while AVCAP did not. This does not establish the behavior of TV mode. The reported endpoint buffer was only 4 ms and unsafe to tap indiscriminately. No offsets from that research are applied here.

- Sony setup: https://www.playstation.com/en-us/support/hardware/3d-audio-ps5/
- Sony TV sound caveat: https://www.sony.com/electronics/support/televisions-projectors-lcd-tvs-android-/xbr-65x930e/articles/00269743
- AVCAP endpoint research: https://github.com/FGGstore/FGG-PlayPods/issues/5
- Independent DualSense microphone calibration research (not Sony Tempest): https://github.com/sainsaji/PS5-Audio-Passthrough-Research

## Set up the 2.1 speakers

1. Connect the CX31993's stereo jack to the active 2.1 system. Keep the PS5 HDMI connected to the screen for video. On PS5 select HDMI/TV as the output/device type and Linear PCM, not headphones, AV amplifier, Dolby Atmos or DTS. Disable automatic output switching if another headset is selected. Mute the display with its own remote or disconnect its speakers, not by using PS5 global mute. Turn off other surround effects. Leave the subwoofer crossover at its normal setting.
2. Launch the bridge manually; first verify playback and hardware volume. Keep the speakers at a moderate level. Do not autoload until tested.
3. In Settings > Sound > 3D Audio (TV), enable 3D Audio for TV Speakers. Use Sony's stereo/3D preview. Check that the *same* preview can be heard through the CX31993, not through the display.
4. Select Measure Room Acoustics for 3D Audio. Hold the unmuted DualSense at your usual seating position. Before accepting a measurement, verify that the measurement tones are actually audible from the 2.1 speakers alone and that Sony's wizard reports success. Abort if tones are absent, come from the TV instead, or break up. A successful wizard alone does not establish that its correction reaches AVCAP. Do not increase the DAC to maximum to force a measurement.
5. Enable Apply 3D Audio Measurement Results. Compare with the setting disabled, keeping 3D Audio enabled.

Sony officially targets TV speakers; a wider separate stereo pair and subwoofer are not guaranteed to match Sony's design assumptions. A normal stereo jack carries L/R; the subwoofer is internally derived and cannot be individually addressed as an LFE channel. Recalibrate if the seating/speaker placement changes. Avoid stacking unrelated surround/EQ/delay.

## Objective A/B test in the actual bridge

The diagnostic captures the incoming source mix, *before* interpolation and USB output, with bounded RAM (up to 3.84 MB) and a disk worker. It never opens the mic; the microphone remains owned by Sony's native measurement workflow. The captured WAV can contain game/system audio; files stay local and are only created on request. Do not capture private voice chat. Only nine fixed filenames are allowed; a repeat replaces its earlier result. Disable the diagnostic and delete WAVs when done.

- Create an empty /data/solobridge/enable_tempest_probe before launching the new ELF. The feature is otherwise off.
- After launch and normal playback, atomically rename a text file containing one label to /data/solobridge/tempest_probe.request (existing FTP/file manager or local shell). Do not queue multiple requests. Wait for /data/solobridge/tempest/status.txt to show complete before changing a setting or sending another request.
- Play a repeatable identical scene/clip with enough pre-roll and 8 seconds of identical audio. Record off_a and off_b with TV 3D disabled, on_a and on_b with TV 3D enabled. Repeat each capture with the same source, timing, level and game settings. Sony's preview may use different material or level; a difference alone is not proof.
- For measurement, submit calibration just before starting the native wizard, then check whether the test tone is audible from the 2.1 system alone. The WAV records 20 seconds. Repeat if timing was missed. If AVCAP has no records or overruns, it reports failure and does not commit a new recording.
- After a successful native measurement, keep TV 3D on and repeat the same clip using measurement_off_a/b and measurement_on_a/b for the Apply Measurement Results switch.

Copy the WAVs to a PC and analyze them (Python 3 + numpy):

    python -m pip install numpy
    python tools/compare_tempest.py PATH_TO_WAV_FOLDER
    python tools/compare_tempest.py PATH_TO_WAV_FOLDER --measurement
    python tools/compare_tempest.py PATH_TO_WAV_FOLDER --calibration

Interpretation: same-setting repeats must align and have high correlation/low residual. If cross-setting residuals are reproducibly higher than repeats, the source is affected by the switch; this is evidence, not proof of the specific renderer. An unchanged source despite reproducibly changed HDMI output is evidence that AVCAP is upstream of the TV processing. Compare an independent HDMI PCM capture of the same deterministic clip if available. A native calibration tone reaching the DAC and wizard success are both prerequisites; also check measurement-result A/B, not only the test tone. A failed repeat leaves any older WAV intact: check status and timestamps before copying results. The fixed capture durations are in source samples, not wall-clock seconds.

If AVCAP lacks the processed TV signal, enabling a menu flag or injecting a stereo filter cannot deliver native Tempest. The reliable alternative to this software bridge is to use the native HDMI/TV output with a TV analog line output or compatible HDMI audio extractor; this bypasses CX31993 unless there is a separate USB host bridge. A post-render HDMI capture path inside PS5 would require firmware-specific research and hardware validation before implementation. No kernel patches or unknown source indices are used by this build.
