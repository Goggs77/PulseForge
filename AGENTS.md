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
Unity round trip (bit-exact), the per-sample audio-rate tanh chain, and the
input-driven Spectrum Analyzer (silent / precomputed / live). When NVENC is
usable it also renders a real GPU export.

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
- Monitor playback follows the same route through `Exporter::renderOutputAudio`
  plus `AudioClip::setPlaybackBuffer` (nullptr = silent, no stream). The app
  rebuilds the monitor when `monitorRouteIdentity` changes while playing, so
  rewiring the Audio Output takes effect immediately; parameter edits apply on
  the next Play. Pre-rendered DAC buffers must not be overwritten by the live
  window path (`evalDac` keeps buffers larger than one video frame).
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
- DAC output buffers are prepared per video frame by `prepareDacBuffer`, which
  keeps the same `audioRenderKey` reuse/append rules as the exporter and never
  mutates a pre-rendered monitor buffer during live playback. Rendered buffers
  record where their first sample sits with `AudioBuffer::startFrame`.
  `Value::carrier` remains only as the fallback for a DAC outside a detected
  region (a frame-rate Scalar applied to a waveform).
- The Spectrum Analyzer is input-driven: it emits no Analysis when its Audio
  port is unconnected, uses `ctx.analysis` only when that port is exactly the
  decoded clip, and otherwise measures a live window with `analyzeWindow`.
  `AnalysisData::originTime` maps that window in time, and
  `AnalysisData::source` feeds the Spectrum block's spectrum/waveform textures
  through `Renderer::uploadAnalysisTextures`. `analysisFrom` treats a connected
  Analysis port as authoritative instead of falling back to the project clip.
- `Project::ensureAudioOutput` upgrades files saved before `out.audio` existed,
  wiring it to the end of the chain the old exporter followed; `pf_migrate`
  applies the same migration to files on disk. Keep both paths working whenever
  the Audio Output or the audio chain changes.
- Project files are JSON (`<name>.pforge`); a UTF-8 BOM is tolerated. Audio and
  video encoder choices are project metadata (`output.audioCodec`,
  `output.videoCodec`), so keep them round-tripping through `Project::toJson`/
  `fromJson`.
- Logging: `SetTraceLogLevel(LOG_WARNING)` is active, so use
  `TraceLog(LOG_WARNING, ...)` for anything a scripted run should be able to see.

## Commits

Conventional commit subjects (`feat:`, `fix:`, `docs:`, `chore:`), one subsystem
per commit, no build output or generated media. `build/` and diagnostics are
already ignored by `.gitignore`. Update `README.md`/`AGENTS.md` in the commit
that changes the behaviour they describe.
