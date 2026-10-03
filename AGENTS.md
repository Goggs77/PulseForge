# AGENTS.md - working agreement for PulseForge

This file tells an automated agent (or a new contributor) how this repository is
built, tested and changed. Keep it up to date: if a change alters a workflow,
update this file in the same commit.

## What this is

PulseForge is a C++20 audio-reactive video synthesiser. Audio is decoded through
the ffmpeg **command line tool** (never linked libraries), analysed on the CPU,
turned into a flow graph of blocks that render through OpenGL render targets, and
exported by piping raw RGBA frames back into ffmpeg. The editor uses raylib with
an immediate-mode widget toolkit (`src/ui/Widgets.*`) plus a few widgets drawn by
the bundled CrystalGUI library (`../crystalgui`).

## Build

Toolchain used on the development machine (Windows, MinGW-w64 + Ninja):

```powershell
cmake -S I:\Goggs\Works\c++\PulseForge -B I:\Goggs\Works\c++\PulseForge\build -G Ninja `
      -DCMAKE_BUILD_TYPE=Release `
      -DCMAKE_CXX_COMPILER=D:/Qt/Tools/mingw1120_64/bin/g++.exe `
      -DCMAKE_MAKE_PROGRAM=D:/Qt/Tools/Ninja/ninja.exe
cmake --build I:\Goggs\Works\c++\PulseForge\build --parallel 8
```

- CrystalGUI is picked up from `../crystalgui` unless `third_party/crystalgui`
  exists; override with `-DCRYSTALGUI_DIR=...`.
- ffmpeg/ffprobe are located on `PATH` (`PF_FFMPEG_EXE`/`PF_FFPROBE_EXE` cache
  entries) and baked into the binary as defaults.
- Binaries land in `build/bin` (`PulseForge.exe`, `pf_selftest.exe`,
  `pf_probe.exe`, `pf_stage.exe`, `pf_migrate.exe`). The app expects `resource/`
  and `assets/` next to the executable, or beside the working directory.
- Full rebuild: add `--clean-first`. Warnings are on (`-Wall -Wextra`) but there
  are a few known, accepted ones (an unused helper in `Registry.cpp`/`Project.cpp`
  and the vendored `jar_mod.h`).

## Tests and verification

Run the self test after every change; it is the single source of truth for
"the app still works":

```powershell
build\bin\pf_selftest.exe "I:\Goggs\Goggsmusic\identity Project\identity_01.flac"
```

It covers decode -> analysis -> in-process rendering -> export -> probe, plus
specific regressions: math/matrix blocks, project save/load round trip, project
metadata, text editing (caret, selection, clipboard), shader ports derived from a
`.glsl` file and `shader.pass` migration, per-family encoder arguments and the
audio matching rules, legacy Audio Output migration, ADC/DAC conversion, and the
exclusive Audio Output routing (processed, dry and silent exports, plus the
monitor's source/silent/processed decisions). It also checks the ADC -> DAC
Unity round trip (bit-exact), the per-sample audio-rate tanh chain, stereo
left/right separation, live frame-window reuse, and the input-driven Spectrum
Analyzer (silent / precomputed / live). When NVENC is usable it also renders a
real GPU export.

It writes `selftest_output.mp4`, `selftest_project.pforge`,
`selftest_legacy.pforge`, `selftest_legacy_audio.pforge`,
`selftest_dynamics.pforge`, `selftest_dynamics.mp4`, `selftest_dry_source.wav`,
`selftest_dry.mp4`, `selftest_dac.mp4`, `selftest_silent.mp4` and (when GPUs are
available) `selftest_nvenc.mp4` into the current directory - delete them when
you are done.

Other tools:

- `pf_stage.exe <audio> <stage> <out.png> [time] [width] [height]` - renders one
  pipeline stage (`--help` is not implemented; the stage numbers are documented
  at the top of `tools/stage_dump.cpp`).
- `pf_probe.exe` - OpenGL/readback probe for render-target experiments.
- `pf_migrate.exe <project.pforge> [...]` - loads projects through the current
  migrations and rewrites them in place, keeping `<name>.pforge.bak`.

### Verifying the UI

The window never takes focus in this environment, so real mouse/keyboard input
cannot be injected. Use the off-screen capture mode instead:

```powershell
PulseForge.exe --shot out.png --size 2000x1200 --shot-frames 40 [project.pforge|audio.wav]
```

- Verify visually with a screenshot plus a numeric check (crop + `ffmpeg
  signalstats`, or `volumeDetect` on exported audio) rather than by eye alone.
- Drive UI state through data where possible (a test `.pforge`, a preferences
  file, command line arguments). If a temporary env-gated hook is unavoidable,
  remove it before committing and grep for `TEMPORARY`/`getenv` afterwards.
- Never leave screenshots, probe files or converted media in `build/bin`, and
  never delete the user's own exports (`output.mp4`, `*.mkv`, `*.webm`,
  `10.*`) or project files.

## Layout

```
src/core     Port/Param model, Node/Graph, Registry (block catalogue), Project
             (metadata + JSON), Json, TextEdit (shared single-line editor)
src/dsp      ffmpeg decode, FFT, analysis, preview audio stream
src/render   OpenGL renderer, shader library, geometry, frame readback
src/export   ffmpeg child process, exporter (frames in, video out)
src/ui       App shell, GraphCanvas, Inspector, Timeline, Preview, dialogs,
             immediate-mode widgets and the theme
tools        pf_selftest, pf_probe, pf_stage
assets       bundled .glsl shaders reachable as "assets/shaders/x.glsl"
docs         rendering notes and the README overlay image
```

## Conventions and invariants

- C++20, four-space indentation, ~100 column lines, `pf` namespace, no
  exceptions in engine code, `#include` order kept as it is in each file.
- Blocks live in `Registry.cpp`. A `NodeDef` carries kind/category/label/
  description/inputs/outputs/params/evaluate. When a kind is **renamed**, add an
  alias to `Registry::modernKindFor` *and* a migration in `Project::fromJson`;
  keep `Project::resetToDefault` on the current kinds. `shader.pass` ->
  `render.spectrum` / `render.shader` is the reference example.
- Ports: types are checked by `portTypeCompatible` (Vec2 widens to Vec3/Vec4 with
  zero fill). A node's effective ports are `Node::inputPorts()/outputPorts()`;
  blocks whose ports follow their configuration store them in
  `Node::dynamicInputs` (the Shader block derives them from the uniforms its file
  uses). Never index `def->inputs` directly - use the accessors.
- Modulation inputs are **appended** to a block's input list: saved projects store
  links by port index, so inserting a port in the middle would re-target them.
  The convention for the value is: rates/frequencies multiply by `2^input`
  (octaves), levels multiply by `(1 + input)` and offsets add the input.
- A block that modulates a parameter **publishes the value it actually used**
  through `Node::publishEffective(key, value)`. The inspector slider and the
  in-block previews follow it (`Node::effectiveParam`) so an LFO visibly moves
  the control instead of leaving it on the base value; the inspector falls back
  to the base value while that slider is being dragged (`ui::sliderDragging`).
- Parameters are `ParamKind` values with an optional `logarithmic` flag (used for
  the Frequency Band and Signal Filter Hz sliders) and an optional
  `valueFormat` for the readout (`"%.0f Hz"`, `"%.2f Hz"`); an empty
  `valueFormat` falls back to `%.3g`, or `%.0f Hz` for logarithmic parameters.
- A block's live content can take mouse input of its own: mark it in
  `nodeVisualHasPivot()` and implement `nodeVisualPivotDrag()` (the Signal
  Filter's cutoff/resonance pivot is the reference). The canvas hit-tests the
  visual rect before the node drag, and `drawNodeVisual` is scissored to the
  block body so nothing can spill out.
- New categories need nothing beyond `def.category`: the palette, its captions
  and the graph accent colour are generated from the registry ("Debug" is the
  most recent addition).
- Sizes *inside* a block's live content (text, handles, strokes) are derived from
  the body rect through `visualFont`/`visualHandle`/`visualStroke`/`visualInset`,
  so they scale with the canvas zoom exactly like the block and never clip.
  `drawNodeVisual` scissorstamps its own body and therefore ends the caller's
  scissor: the canvas re-establishes its viewport clip right after the call.
- UI text goes through the atlas helpers: `ui::drawText`, `drawTextClipped`,
  `drawTextWrapped`/`textWrappedHeight`. Do not use CrystalGUI's
  `CguiDrawTextPro`; it renders thin strokes and wraps with a guessed line count.
- CrystalGUI draws the chrome buttons, but **the app owns their input**
  (`updateChromeInput` in `App.cpp` hit-tests the cached rects). Its event
  dispatch used to fire the wrong block's callback. Leave
  `canHandleMouseEvents = false` on chrome nodes.
- Text entry uses the one shared editor (`src/core/TextEdit.*` plus the glue in
  `Widgets.cpp`). A press that ends an edit must be followed by an
  `gEdit.active() && gEdit.id == id` check before the buffer is read, otherwise
  the field commits an empty string.
- Audio preview: raylib streams through a double buffer.
  `SetAudioStreamBufferSizeDefault` sets the *sub-buffer* size and
  `UpdateAudioStream` must write exactly one sub-buffer (2048 frames) per call,
  primed after `PlayAudioStream`; anything shorter is zero-filled and sounds like
  periodic silence.
- Import decodes at the source sample rate. `chooseAudioEncoder`
  (`core/Project.cpp`) matches the export codec and bitrate to the source; PCM
  sources become FLAC inside Matroska (players otherwise output silence), and a
  source with no matching encoder is converted to AAC **in memory** and copied on
  export.
- Export always goes through `Exporter::buildCommand`. Quality options are
  per-family (`-crf` for x264/x265/SVT-AV1, `-cq` for NVENC, `-global_quality`
  for Quick Sync, `-qp_i/-qp_p` for AMF). `-af apad` plus `-shortest` keeps a
  short audio track from cutting the video. GPU encoders are probed with
  `ffmpeg::canRunVideoEncoder` before a run, and
  `exportPathForContainer` keeps the file extension in step with the container.
- The export's soundtrack is **exclusively** the signal wired into `out.audio`.
  A direct link from `src.audio` muxes the media file with its matched encoder;
  any other source node must fill `Node::audioRenderOutput`/`audioRenderFrames`
  while the graph is evaluated forward (Dynamics, DAC). `Exporter` renders that
  path one video frame at a time with the renderer detached (so modulation still
  applies), writes it to a temporary 32-bit float WAV and muxes that; the render
  pass then reuses the same buffer through `audioRenderKey`. A missing or
  unconnected Audio Output exports no audio track. Keep the per-frame window
  logic contiguous so this path stays sample-exact, and keep `buildCommand`'s
  `audioSeek` at 0 for the rendered WAV (it already starts at the export offset).
  `buildCommand`'s `muteAudio` flag is what turns an empty override into `-an`;
  without it the empty path would fall back to the media file.
- Monitor playback follows the route live: `prepareMonitorAudio` starts
  `AudioClip::startLiveStream` for a processed chain and the app pushes the DAC
  window after every preview render, so rewiring the Audio Output or editing a
  parameter is heard on the next displayed frame. `Exporter::renderOutputAudio`
  remains the offline per-frame renderer used by export.
- ADC/DAC define **audio-rate regions**. `Graph::evaluate` detects every
  ADC -> Scalar -> DAC path with `buildAudioRatePlan` and evaluates its
  pure-Scalar nodes (Math, Modulation, Timing, Debug) once per audio sample:
  ADC reads the waveform sample, the intermediate blocks transform it, and DAC
  writes it back at the same rate. Pure-Scalar processors feeding the region run
  at audio rate too; window producers (Spectrum Analyzer, Dynamics) keep their
  frame-rate evaluation and hold their value inside the region. Nodes downstream
  of the region run after the sample loop and see the last sample, which is the
  downsample back to video rate. `EvalContext::audioRate`/`audioSampleRate` and
  `rateOf()`/`dtOf()` are how rate-aware blocks pick their timing; do not read
  `ctx.fps` directly in a block that can sit inside a region.
- ADC/DAC are multi-channel: the `channels` parameter (1..8, default 2) builds
  dynamic Scalar ports named left/right/ch3... via
  `Registry::applyChannelPorts` (`Node::dynamicOutputs` for ADC,
  `Node::dynamicInputs` for DAC). `Graph::evaluate` re-syncs them when the
  parameter changes and drops links to ports that no longer exist. A DAC
  channel with no link follows the first connected channel, so a legacy
  single-port chain stays dual-mono instead of losing a side.
- Audio sources inside a region are explicit: `sampleAdcNode` and `evalAdc`
  never fall back to `ctx.audio` when the Audio input is unconnected, because
  that is silence. Keep the graph walk tied to the actual Audio Output path.
- Audio-rate blocks must stay O(1) per sample. Ringbuffer keeps a
  `Node::ringValues` ring with `ringSum`/`ringHead` and only refreshes its
  visual history once per video frame; do not reintroduce per-sample O(N) sums
  or full-buffer shifts. Dynamics uses a linear-domain static curve
  (`detector^exponent * factor`) instead of a log10 + pow pair per sample.
- Resolution-modulating blocks must not reallocate targets per frame: Spectrum
  quantises the modulated scale and keeps its feedback texture at the base
  scale, and `Renderer::acquire` caps the target pool at 12 with LRU eviction.
- Geometry (`geom.primitives`) owns primitives only and must not read
  `ctx.audio`/`ctx.analysis`. Spectrum and waveform shapes are
  `render.spectrum` effects; `Project::fromJson` converts old Geometry shapes
  3..6 to Spectrum (`radial_spectrum`/`bars`/`waveform_scope`), remaps the old
  Scale input to the Spectrum Scale port and wires the project analyzer to the
  Analysis input. `remapGeometryShape` shifts the remaining primitive enum
  values (old 7..10 -> new 3..6). Keep `pf_migrate` in step, and never
  reintroduce Analysis/Audio inputs or global-analysis use in Geometry.
- DAC output buffers are prepared per video frame by `prepareDacBuffer`, which
  keeps the same `audioRenderKey` reuse/append rules as the exporter and never
  mutates a buffer currently streamed by the monitor. Rendered buffers record
  where their first sample sits with `AudioBuffer::startFrame`.
  `Value::carrier` remains only as the fallback for a DAC outside a detected
  region (a frame-rate Scalar applied to a waveform).
- The Spectrum Analyzer is input-driven: it emits no Analysis when its Audio
  port is unconnected, uses `ctx.analysis` only when that port is exactly the
  decoded clip, and otherwise measures a live window with `analyzeWindow`.
  `analyzeWindow` ends at the current video frame's end, so the analysis matches
  the samples the region produced in the same evaluation.
  `AnalysisData::originTime` maps that window in time, and
  `AnalysisData::source` feeds the Spectrum block's spectrum/waveform textures
  through `Renderer::uploadAnalysisTextures`. `analysisFrom` treats a connected
  Analysis port as authoritative instead of falling back to the project clip.
- Monitor playback follows the Audio Output live: `AudioClip::startLiveStream` /
  `pushLiveWindow` queue each video frame's rendered window. The sub-buffer is
  sized to at least one video frame's sample count *and* the real device
  sub-buffer (`detectStreamSubBufferFrames` probes it, because raylib silently
  raises a small request to the device period and zero-fills the remainder of
  every update); two sub-buffers are prefilled before playback so the device
  cannot drain the ring between video frames. The app pushes the DAC buffer right after
  `renderPreviewFrame`, and `catchUpLiveAudio` renders any project frames a slow
  display skipped, in order. The region quantises its window to `ctx.frame` so a
  high-refresh display reuses the same window instead of advancing stateful
  blocks several times per project frame. Export still renders the same
  per-frame pass offline and muxes the resulting track.
- `Project::ensureAudioOutput` upgrades files saved before `out.audio` existed,
  wiring it to the end of the chain the old exporter followed; `pf_migrate`
  applies the same migration to files on disk. Keep both paths working whenever
  the Audio Output or the audio chain changes.
- `Project::resetToDefault(const ShaderLibrary *)` loads `template.pforge` next
  to the executable (with the current working directory as a second candidate):
  the template is the editable default pipeline used by startup and *File >
  New*. It clears `filePath` so the new document is unsaved, and the built-in
  pipeline remains the fallback when the template is missing or invalid.
  `newProject` passes the renderer's ShaderLibrary so Shader blocks derive their
  ports, and clears stale preview media when the template has no audio.
- Project files are JSON (`<name>.pforge`); a UTF-8 BOM is tolerated. Audio and
  video encoder choices are project metadata (`output.audioCodec`,
  `output.videoCodec`), so keep them round-tripping through `Project::toJson`/
  `fromJson`.
- Logging: `SetTraceLogLevel(LOG_WARNING)` is active, so use
  `TraceLog(LOG_WARNING, ...)` for anything a scripted run should be able to see.

### Audio stream sizing (raylib 5.6-dev, raudio 1.1, miniaudio 0.11)

PulseForge uses the CrystalGUI submodule's raylib (`raylib.h`: major 5, minor 6,
patch 0, `RAYLIB_VERSION "5.6-dev"`), whose audio layer is raudio 1.1 on
miniaudio 0.11. On Windows miniaudio selects **WASAPI shared mode** by default;
the device period is chosen by the OS/driver and is not exposed through
raylib's public API.

- `SetAudioStreamBufferSizeDefault(N)` only *requests* a sub-buffer size.
  `LoadAudioStream` allocates a 2x ring (`sizeInFrames = subBufferSize*2`) and
  raises `subBufferSize` to `AUDIO.System.device.playback.internalPeriodSizeInFrames`
  when `N` is smaller. `AudioStream.buffer` is opaque, so the effective size
  cannot be queried.
- `UpdateAudioStream(stream, data, frameCount)` copies `frameCount` frames into
  one sub-buffer and **zero-fills the remaining `subBufferSize - frameCount`**
  frames. Feeding a frame-sized chunk smaller than the real sub-buffer is
  audible as periodic silence and dropped audio; this was the October 2026
  preview bug.
- Measured on the development machine (44.1/48 kHz WASAPI): with a 1024-frame
  request, writing 1537 frames succeeds but 2049 warns `STREAM: Attempting to
  write too many frames to buffer`, so the real sub-buffer is ~1537-2048 frames
  (about 40 ms; ~1920 frames at 48 kHz). `detectStreamSubBufferFrames` in
  `src/dsp/AudioClip.cpp` exploits that warning: it installs a trace-log
  callback, loads throwaway streams with `defaultSize = 1`, binary-searches the
  largest writable frame count and caches the result per sample rate. If the
  warning text changes, it falls back to 16384 frames and logs a warning rather
  than feeding a too-small buffer.
- `AudioClip` streams at `max(one video frame's samples, detected device
  sub-buffer)`, prefills two sub-buffers for live playback and feeds exactly the
  detected size. The buffered (direct Audio Source) stream uses
  `max(2048, detected)` for the same reason.
- Do not "fix" an underrun by feeding smaller chunks (that zero-fills), and do
  not try to read `AudioStream.buffer` (it is not public). Re-measure the
  warning string and period handling when raylib/raudio/miniaudio is upgraded.
- **Device-wise config is not required.** The period is probed per process and
  sample rate, so another machine or output device adapts on first playback.
  Live playback re-probes on every Play (the buffered direct-source stream
  reuses the cached value), so switching the default output device while the
  app is running is picked up on the next Play. A user-facing buffer override
  could be added later, but correctness does not depend on a per-device table.

## Commits

Conventional commit subjects (`feat:`, `fix:`, `docs:`, `chore:`), one subsystem
per commit, no build output or generated media. `build/` and diagnostics are
already ignored by `.gitignore`. Update `README.md`/`AGENTS.md` in the commit
that changes the behaviour they describe.
