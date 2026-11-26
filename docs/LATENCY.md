# Low-latency plan (PS5 13.00, TTGK CX31993)

## Targets
60 fps = 16.667 ms. Aim for 5–8 ms additional delay, preferably steady;
no universal perceptual threshold. Do not equate packet interval or queue
depth with actual action-to-sound or audio-video latency. References:
- https://nime.org/proc/nime2016_mcpherson/
- https://online.ucpress.edu/mp/article-abstract/36/1/109/92063/
- https://aes.org/publications/elibrary-page/?id=12881
- https://ekho.csail.mit.edu/content/ekho-sigcomm.pdf

## Evidence and decision matrix
- AVCAP: upstream FGG-PlayPods source documents 1024 frames/record, source 2,
  raw float stereo. Read capacity is not producer grain. Test 512/256/128
  with full-size physical allocation, count actual frames and read intervals;
  if errors or insufficient throughput, reject. No guessed OpenAudio fields.
- Polling: 2ms control, 0.5ms quick, prediction with 2ms guard and 10ms max
  sleep. No unverified blocking call or wait handle.
- USB: 8/16/24/32 frames x2, compare previously verified 64x3 and write.
  Three calls per steady completion target (select, COMPLETE, START).
  Each frame remains exactly 36 bytes, validate every completion.
- Ring: 96-frame reserve versus 192, trim muted startup preroll only, count
  that separately. Adaptive underrun recovery retained. Never claim lossless
  runtime if counters increased. Producer memcpy and batch read publication.
- PCM: SSE2 candidate preserves exact output bytes and interpolation; no
  fast-math, sample rate or bit-depth reduction.
- CPU: whole process user/system; optional individual thread CPU; source
  sample age up to 250ms. Does not include other system processes or interrupt
  work charged elsewhere.
- Native: standard FreeBSD 11 uaudio has UAC2 code, but this proves nothing
  about Sony's binary or policy. Query interface driver before configuring
  DAC; no kernel writes, detach or registration. See
  https://github.com/freebsd/freebsd-src/blob/releng/11.0/sys/dev/sound/usb/uaudio.c
  and https://github.com/FGGstore/FGG-PlayPods/issues/5 .
  Public evidence does not establish a native PS5 UAC2 path on firmware 13.00.
- Tempest/calibration: unchanged.

## Measure actual relative delay
Connect a line-level HDMI PCM reference (e.g. audio extractor) to left input
of a stereo ADC, TTGK analog output to right input of that same ADC. Set
levels safely, avoid clipping, do not short outputs or connect speaker-level
signals. Capture both simultaneously, same clock, PCM16 stereo WAV, using
broadband nonperiodic audio. Disable any processing on the recording path.
Use an identical recording setup for each profile.
Run on PC: python -m pip install numpy, then
python tools/measure_latency.py recording.wav
Positive delay = TTGK later than HDMI. Reference extractor has its own delay;
measure it separately if absolute accuracy matters. Video/display latency
requires a photodiode or simultaneous high-speed A/V measurement.
The in-panel queue p95 is sampled, not an actual latency percentile.
No mode is permanently selected by the benchmark; choose after real console
results, audible verification and sustained game stress test.
