# Audio playback and A/V timing inspection

Status: audio playback, waveform inspection, automatic transients, marker
interaction/persistence, and timing audits implemented. External audio, headless
export, persistent waveform caches, and native output-latency calibration remain
extensions. The current output clock is an SDL consumption estimate; device loss
is reported if SDL cannot migrate the default output.

## Recommendation

Use the existing FFmpeg and SDL3 stack. Add synchronized audio playback and
an optional waveform strip below the video. Enable the strip through the CLI.
Keep waveform inspection useful while paused and muted, with independent
horizontal zoom, channel lanes, frame boundaries, and persistent timing markers.

The assumed meaning of CLI visualization is a flag that opens an interactive
panel. Headless export can use the same waveform and measurement model later.

## Proposed CLI

```sh
hdrplay --audio clip.mov
hdrplay --audio-waveform clip.mov                  # silent timing inspection
hdrplay --audio --audio-waveform clip.mov          # inspection with sound
hdrplay --audio --audio-waveform --audio-source 1:a:0 master.mov encode.mov
hdrplay --audio-waveform --markers sync.json clip.mov
hdrplay --audio-waveform --audio-transients --markers sync.json clip.mov
```

- `--audio`: play the selected audio track in sync with video.
- `--audio-waveform`: reserve a waveform strip below the video. Independent
  of the existing `--waveform`, which displays RGB signal levels.
- `--audio-source N:a:M`: select audio track ordinal M in input N, both
  zero-based. Default to the first audio track in input 0. Use this selection
  for the waveform and sound; never change it implicitly when panes swap.
- `--markers FILE`: load/save an explicitly selected JSON measurement sidecar.
  Include a schema version and source identity; verify identity before applying
  saved markers to a different file.
- `--audio-transients`: automatically mark only clear impulsive audio onsets;
  implies `--audio-waveform`. Run detection on the selected source audio.

Audio remains opt-in. Explicitly requesting an unavailable track produces a
clear error. Later additions can include external WAV input, two-track waveform
comparison, CLI marker creation, and headless JSON/CSV/image export.

## Inspection workflow

1. Pause on the visual event, such as the first frame showing a clap closing.
2. Add a video marker at the displayed frame's presentation timestamp (PTS).
3. Zoom the waveform around that frame and select the audio transient onset,
   or use an automatically detected transient marker.
4. Add an audio marker snapped to a decoded sample, or enter an absolute time.
5. Read the signed difference and save the marker pair.

The strip shows a current-time playhead and highlights the entire interval
occupied by the displayed video frame. Zoomed out, label selected frame
boundaries; zoomed in, show each boundary. Clicking the ruler seeks video;
dragging or zooming the waveform viewport does not change the media position.
Waveform pointer events must take priority over the existing video probe and
scope-region gestures. Frame stepping stays silent initially.

Draw channels separately, with channel names, a zero line, and fixed amplitude
scaling. Combining stereo channels by summation can hide a transient through
phase cancellation. Downmixing for speakers must not change the measurement
waveform.

## Conservative automatic transient markers

Automatically mark clear impulses such as a clean clapboard strike. Prefer
missing an ambiguous event over placing a misleading marker. Detection identifies
an audio transient; it cannot establish that the sound came from a clapboard or
identify the matching visual frame. Pairing with video remains explicit.

Use a deterministic detector over original decoded PCM, independent of playback
gain, device resampling, and waveform zoom. Analyze channels separately. A clear
event in one channel can qualify; stereo agreement is useful supporting evidence
but is not required, since microphones may have different levels and arrival
times. Retain each qualifying channel's onset and merge nearby detections for
navigation without averaging their timestamps. Use the selected channel's onset
for a measurement, and show which channel supplied it.

Require all of the following:

- Strong contrast against a robust local noise/background estimate, with an
  absolute level floor so tiny numerical spikes in silence do not qualify.
- A very fast attack and a distinct onset following a quiet baseline.
- High peak-to-background and peak-to-local-energy ratios, consistent with an
  impulse rather than a gradual swell or sustained tone.
- Evidence of a broadband attack, helping reject ordinary tonal onsets.
- A single resolvable attack. Group the strike and its ringing/echo tail into
  one event; abstain if multiple attacks make the onset ambiguous.

Starting thresholds to tune against fixtures and recordings: at least 20 dB
above the preceding 50 ms background, a peak of at least -18 dBFS, an attack
within 2 ms, and at least 12 dB crest factor in a short event window. Specify
the envelope/window definitions and broadband gate during implementation.
These are deliberately strict initial settings, not validated guarantees or
universal clapboard characteristics. Avoid a sensitivity slider initially;
version the detector and its parameters so saved results are reproducible.

Find candidates in short-window energy/attack features, then refine accepted
onsets in a bounded PCM neighborhood. Place the marker at the first sustained
departure from the baseline, not the loudest sample or the center of an analysis
window. Convert the chosen source sample through the same timestamp mapping as
manual audio markers. Account for analysis/filter delay if used. Save an onset
uncertainty interval; sample-addressable placement does not guarantee knowledge
of the original acoustic onset, especially with lossy-codec pre-echo.

Run in the background alongside waveform generation, with overlap across decode
blocks so block boundaries cannot duplicate or hide events. Reset the detector
at timestamp gaps/discontinuities; decoder failures and damaged boundaries must
not become apparent strikes. Timestamp and identify events consistently across
viewport changes, seeks, and analysis chunk sizes.

Display automatic markers distinctly, allow previous/next transient navigation,
and let the user accept, adjust, dismiss, label, or pair each one with a frame.
Automatic markers are already present on the waveform; acceptance records a
reviewed measurement. Persist detector version, parameters, channel evidence,
onset uncertainty, and review state. Preserve edits and dismissals on reanalysis;
never replace a manual marker or silently select the nearest video frame as a
confirmed A/V event.

Verification includes clean and quiet claps, background speech/music, drum hits,
tonal starts, fades, clicks, clipped impulses, echoes, stereo delay/phase
inversion, codec pre-echo, and events crossing decode boundaries. Sharp drum hits
or clicks may correctly qualify as transients without being clapboards. Measure
false positives and onset error, and require rejection of ambiguous fixtures.

## Timing model: actual timestamps

Use one common timeline origin per input for both audio and video. Prefer the
container presentation origin when valid, otherwise use a documented shared
fallback from available stream timestamps. Never independently subtract each
stream's start timestamp: that would erase real A/V offsets. Preserve original
integer PTS and rational time bases alongside display-friendly elapsed seconds.
"Absolute time" means a position on this media timeline, not wall-clock UTC.

The local `1_10bit_hlg.mp4` probe reports video starting at 0 and stereo AAC
audio starting at 0.001 s, with 48 kHz audio. Preserve that 1 ms difference.

Audio samples map to their decoded block's presentation timestamp plus the
sample offset divided by sample rate. Honor decoder/demuxer handling of codec
priming, skip samples, end padding, and container edits without applying trims
twice. Preserve timestamp gaps and distinguish missing data from actual silence.
Resampling delay belongs in the playback mapping, not the source measurement.

### Recovery and length audits

Some sources stamp audio on a coarser cadence than their declared time base.
The observed `/Users/jblome/Downloads/video.mp4` has 48 kHz AAC blocks containing
1024 samples but a 16/24/24 ms PTS-step pattern. A bounded 5.333 ms overlap is
reported as a file-timing warning and recovered using the native sample clock,
preserving its initial 185.604 ms offset. Measurements are labeled inferred and
carry the observed timestamp uncertainty. Timestamp rounding within half a
time-base tick is also handled. Recoverable jitter is bounded to half a decoded
block; larger discontinuities remain explicit rather than being hidden.

The audio audit reports post-decoder sample count / native sample rate, actual
presentation span, gaps, declared audio duration, and signaled priming/trailing
skip samples. FFmpeg handles explicit skips; they are not applied twice. Compare
presentation span against stream duration, accounting for timestamp precision.
When complete and unambiguous, compare the decoded A/V endpoint against the
container duration. Additional uninspected tracks make that comparison partial.

For the observed clip, 791,552 decoded samples imply 16.490667 s PCM; the stream
declares 16.480000 s. Report the 10.667 ms difference. It fits within one AAC frame,
so unconfirmed trailing padding is a possible explanation, not a reason to trim
valid samples silently. Recovery and duration warnings are visible in the log,
waveform strip, and JSON sidecar.

Frame boundaries come from decoded presentation timestamps, including frame
reordering. The current HUD uses `round((PTS - start) * nominal_fps)` for its
frame number. That cannot serve as an exact ordinal on variable-rate video.
Build a presentation-order frame index for exact frame lookup. Until the index
is available, markers remain exact by PTS and any nominal frame label is marked
approximate. Indexing a long video can require a full decode; perform it in the
background, cache it, and support cancellation. Do not promise immediate exact
frame-number lookup after an arbitrary seek.

For a paired video and audio event:

```text
delta = audio_event_time - video_frame_start_time
positive: audio is late; negative: audio is early
```

Report milliseconds and, where useful, source-sample intervals. Report fractional
frames only with an explicit rate for constant-rate media. For variable-rate
media, use actual crossed frame boundaries and the containing frame interval.
Show differences between arbitrary markers as well as designated A/V pairs.

A selected video event is only localized to a frame interval. Sample-level
audio placement does not make the visual event sample-accurate. Offer the
corresponding offset range using that frame's start and end boundaries.

Marker records retain input/stream identity, kind, label, original PTS/time
base, and a sample offset or verified presentation-order frame ordinal where
applicable. Arbitrary-time markers retain their explicit timeline coordinate.
Do not save only rounded milliseconds or screen coordinates. A/B measurements
use the existing common elapsed playback time, while retaining each input's
origin and original timestamps for reproducibility.

## Audio playback architecture

- Add an audio decoder using libavformat/libavcodec and libswresample. Convert
  playback PCM into one fixed float format, then use SDL3 AudioStream for the
  device. Add libswresample to CMake; it is already installed locally.
- Initially open a separate demux context for audio from local seekable files.
  `decoder_next_frame()` currently discards every non-video packet. An
  independent audio reader avoids changing video probes, seeks, and analysis
  into a coupled demux pipeline. A separate reader for background waveform
  work avoids seeking the playback reader. This adds file reads; a shared
  demux with bounded packet queues is a later option for network/live inputs.
- Decode/refill on a worker using bounded PCM storage. Keep file I/O and
  decoding out of the audio callback. Use seek generations so old worker
  results cannot re-enter a cleared playback queue.
- Introduce a playback-clock abstraction. With sound enabled, video follows
  audio consumption, using a timestamp anchor, queued PCM, monotonic
  interpolation, and a downstream-latency estimate. With sound disabled,
  retain the monotonic clock. Muting active playback must preserve its clock.
- SDL queue consumption is not speaker presentation: downstream device and OS
  buffering remain. Display the clock's estimated precision. Validate wired
  output first; native output timestamps/calibration are an extension if exact
  acoustic/display synchronization is required.
- Pause freezes the timeline and pauses output. Seek/loop pauses output, clears
  PCM/converter state, seeks and prerolls audio, trims to the target, primes
  buffers, aligns video, and reanchors before resuming. Device-buffered sound
  may have a short residual tail; queue clearing cannot retract emitted audio.
- Handle audio starting late with scheduled silence. If audio ends before
  video, hand over continuously to the monotonic clock. If audio lasts longer,
  hold the final video frame until audio finishes. EOF and seek limits must
  include the selected audio timeline.
- On sustained decode starvation, expose buffering and reanchor playback;
  never silently continue reporting sound as synchronized. Track audio
  underruns separately from timestamp gaps in the file.

`source_advance_to()` already selects video by timestamp and can remain the
consumer of the master clock. It currently decodes and measures every frame
on the main thread. Audio mode must handle falling behind: measure decoded
frames but omit obsolete presentations, report lag, and buffer if decoding
cannot catch up. Full exhaustive inspection remains available headlessly.

## Waveform storage and rendering

Build per-channel min/max envelopes at multiple resolutions in a background
worker. Min/max preserves short transients that averaging or RMS can hide.
Select a level near one envelope bin per screen pixel. For close inspection,
decode a bounded source-PCM window and render individual samples. A coarse
envelope is for navigation, never the basis for sample-exact marker placement.
Retain timestamped segment boundaries when audio has gaps or discontinuities.

Cache peaks and frame indexes using source identity and selected stream. Bound
memory and disk use; do not retain a complete long recording as float PCM.
For scale, one hour of stereo 48 kHz float PCM takes about 1.38 GB.

Add a reserved bottom rectangle to `layout.c`, leaving video aspect-ratio and
probe mapping decisions there. Render the panel through the existing HUD
texture/libplacebo overlay path. Cache waveform raster data separately from
the playhead and marker overlays. Redraw while the playhead advances, even if
the video frame has not changed. Start with a short window around the playhead
and allow a pinned viewport for comparing markers while paused.

## Delivery order and verification

1. **Measurement:** timestamp model, background audio peaks, waveform panel,
   actual frame grid, marker pairs, conservative transient detection, and JSON
   persistence.
2. **Playback:** audio decoder/output, audio clock, pause/seek/loop, buffering,
   device-loss recovery, and audio-aware end-of-file behavior.
3. **Extensions:** external audio, two-track comparison, headless export,
   optional preview timing compensation and platform latency calibration.

Verify the measurement model with generated flash/click files containing known
positive and negative offsets, nonzero origins, variable-rate frames, stereo
phase inversion, and timestamp gaps. Verify AAC priming and trim behavior
against a PCM reference. Exercise repeated seek/pause/resume/loop to detect
stale queued audio and long playback to detect drift. Test persistence against
source replacement, and validate that waveform layout preserves video/probe
geometry. Listening confirms basic playback; physical output latency needs a
loopback or recording measurement and cannot be inferred from file markers.

## Alternatives considered

- **External ffplay/mpv for sound:** useful for manual experiments, but separate
  clocks and seek commands make integrated timing and transport unreliable.
- **Embed libmpv:** mature playback and synchronization, but adds a substantial
  dependency alongside the existing custom FFmpeg/libplacebo renderer. Revisit
  if the product expands into general playback, speed changes, and many inputs.
- **Predecode all audio:** simplest for tiny test clips, but unbounded storage
  and startup cost make it unsuitable as the general approach.

The FFmpeg + SDL3 route fits this tool. The principal implementation risk is
the audio clock and transport lifecycle, rather than drawing the waveform.
