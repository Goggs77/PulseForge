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
- The startup banner prints the version and the build time. The `build_stamp`
  target runs `cmake/BuildStamp.cmake` on every build to rewrite
  `build/generated/build_stamp.h` (`PF_BUILD_STAMP`), which is why the app
  relinks even when no source changed; `main.cpp` includes it when present and
  falls back to `__DATE__ " " __TIME__` otherwise (an IDE project, a quick g++
  run). Change the version string in `main.cpp` itself.
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
monitor's source/silent/processed decisions). The Math checks include Lerp/Clamp
(port values versus Inspector fallbacks) and the Transform block's
pass-through/zoom/translation, and the Shader check compiles the three bundled
templates. Automation's Slice mode is checked numerically (holds the first/last
value outside the window and mirrors when reversed), and the Sorting check
covers the group's layered order (compacted depths, row rules), the member list
parsing/formatting and a Sticky Note/Group project round trip. It also checks
the Picture block (a BMP with a red top and blue bottom decodes the right way
up), the Textbox (white glyph over a kept Layer) and that a two frame gif
advances with the timeline, and the meter's ballistics (a VU that averages and
releases slowly, a digital meter that stays raw). The Self Reference check paints
a known pattern into the window, captures it, and asserts the image is upright,
absent before the first capture and handed out without a copy. It also checks
that a processed Analysis input measures the same window in preview and export
(and that the passthrough bridge accepts an inline meter but not a processor),
and that the preview starts from a fresh window after an export. It also checks
the ADC -> DAC
Unity round trip (bit-exact), the per-sample audio-rate tanh chain, stereo
left/right separation, live frame-window reuse, and the input-driven Spectrum
Analyzer (silent / precomputed / live). It also checks that a Spectrum preset
change rebuilds its ports/parameters and re-matches the links by name, that a
migrated Geometry element actually draws, that the waveform display fills the
bar and its rolling history reaches the export span, that a Dynamics feeding an
ADC -> DAC region stays continuous across a high-refresh preview, and that a
modulated Dynamics pre-gain ramps instead of stepping. When NVENC is usable it
also renders a real GPU export.

It writes `selftest_output.mp4`, `selftest_project.pforge`,
`selftest_legacy.pforge`, `selftest_legacy_audio.pforge`,
`selftest_dynamics.pforge`, `selftest_dynamics.mp4`, `selftest_dry_source.wav`,
`selftest_dry.mp4`, `selftest_dac.mp4`, `selftest_silent.mp4`,
`selftest_media.mp4` and (when GPUs are available) `selftest_nvenc.mp4` into the
current directory - delete them when you are done.

Other tools:

- `pf_stage.exe <audio> <stage> <out.png> [time] [width] [height]` - renders one
  pipeline stage (`--help` is not implemented; the stage numbers are documented
  at the top of `tools/stage_dump.cpp`).
- `pf_stage.exe <project.pforge> <out.png> [time] [width] [height]` - renders one
  full-size frame of a real project (defaults to its video size) and dumps every
  intermediate Image as `node_<id>_<kind>.png`, which is the quickest way to
  compare a migrated project against an older export.
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
- Ports that *are* the value rather than a modulation of it (Lerp's
  A/B/Factor, Clamp's In/Min/Max, Transform's Amount and pre-offset) follow the
  other convention: a connected Scalar port replaces the matching Inspector
  value and `publishEffective` makes the slider follow it, while an unconnected
  port falls back to the Inspector. `scalarOrParam` in `Registry.cpp` is the
  helper; keep both directions working so a block is usable with or without
  patch cables.
- A block that modulates a parameter **publishes the value it actually used**
  through `Node::publishEffective(key, value)`. The inspector slider and the
  in-block previews follow it (`Node::effectiveParam`) so an LFO visibly moves
  the control instead of leaving it on the base value; the inspector falls back
  to the base value while that slider is being dragged (`ui::sliderDragging`).
- Automation's **Slice mode** maps the curve onto the Start..Finish window in
  `ctx.time` (the same base as the timeline ruler): `(time - start)/(finish -
  start)` clamped to 0..1, so outside the window the output holds the curve's
  first or last value and a Start past the Finish mirrors the curve (a negative
  span). `evalAutomation` still publishes `runtimeState["pos"]` as the slice
  position, which is what the in-block curve pivot and the inspector's curve
  editor follow. The timeline draws one lane per slice automation
  (`TimelineWidget.cpp`) with a Start->Finish direction arrow and two draggable
  handles; dragging writes the `start`/`finish` params, snapped to video frames,
  and `UiState::sliceDragNode`/`sliceDragEnd` carry the drag between frames. The
  `sliceColor` param colours the lane and its handles so several slices stay
  apart, and a slice is drawn from `min(start, finish)` so a reversed one still
  fills the lane it covers. The lane list scrolls (`UiState::timelineScroll`,
  wheel anywhere under the timeline header or the scroll bar to the right of the
  lanes), so more automations than the panel is tall stay reachable; the lanes
  are scissored to their viewport and a lane that is half scrolled out only
  takes clicks on its visible part. Lane names that still match the block label
  get the block id appended ("Automation #17"), because several automations
  usually share the default name. Slice and Loop are mutually exclusive in
  practice: the slice mapping wins when both are on.
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
- **Sorting** (`sort.sticky`, `sort.group`) is drawn by the canvas instead of
  the generic block path: both blocks have no ports and no evaluate, they shade
  as translucent greyscale cards that contrast against the theme - a light card
  with deep grey ink on the dark theme, a dark card with light grey ink on the
  light theme, a quarter-transparent heading and a semi-transparent body - and
  they never show the "category + ms"
  footer. A group's frame is *derived*: `groupFrame` sizes it from the arranged
  members and `layoutGroup` writes their positions every frame in
  `syncGroups`, so the arrangement is never a saved user layout - only the
  `members` id list is persisted (a text parameter). The drop rules live in the
  canvas: a block dropped with its centre inside a frame joins it, one dropped
  outside leaves, groups never nest (a group id in a member list is pruned), and
  `syncGroups` also drops ids of deleted blocks and of blocks another group
  already claims. The layered order itself (`blockDepth` + `groupLayerOrder` in
  `core/Sorting.cpp`) is pure graph logic so the selftest can check it: columns
  are the compacted depths, rows sort by depth, connected-port count and id.
  Keep the geometry (`nodeWidth`/`nodeTotalHeight`) in the UI and the ordering
  in core - the split is what makes the layout testable without a canvas.
  `sortingColours` in `NodeVisuals.cpp` is the single source for the card and
  ink shades, so the canvas and the note text can never drift apart.
  A group's `depthTolerance` widens a layer to `tolerance + 1` consecutive
  depths (`band = depth / (tolerance + 1)`, bands compacted into columns): a
  quantised merge, never a transitive one, or every depth in a chain would
  collapse into the first layer.
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
- A `Param` marked `multiline` (the Sticky Note's text) is edited with
  `ui::textArea`: the same shared editor, but Enter inserts a newline instead of
  finishing the edit, Up/Down walk the *wrapped* lines, and the box scrolls
  vertically with the caret. `layoutEditLines` is the single layout used by the
  drawing, the caret and mouse placement - keep them on it, or clicking will
  land between two different wrap models.
- Caret, selection and click x positions inside a wrapped line are measured from
  that line's own start (`editColumnX`), never from the buffer start: a caret on
  the second line otherwise inherits the width of the lines above it and is
  drawn off the box (that was "the caret moves down but not left after Enter").
- `TextEditKeys::allowNewlines` decides whether Enter finishes the edit or breaks
  the line, and whether a paste keeps its newlines; single-line fields still
  strip them. `TextEditState::remember`/`undo` give Ctrl+Z one step per typing
  run (consecutive typed characters share it) or per discrete edit, with the
  line break as its own step; the undo stack is capped at 64 entries.
- Any `ParamKind::File` parameter draws a Browse button under its path box; the
  dialog's `node-file` purpose writes the chosen path into
  `BrowserState::paramKey` on the selected node, using the parameter's `hint` as
  the extension filter.
- `Node::pushHistory` keeps the newest sample at the *back* of the buffer and
  `Node::historyAt` walks back from there (position 1 = newest, 0 = oldest).
  It used to index from the front, which mirrored every in-block diagram and
  made the digital meter's readout show the oldest retained sample.
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
- `Exporter::audioRoute` also reports `Source` for a **passthrough bridge**: a
  Unity ADC whose channels reach a DAC that feeds the Audio Output untouched.
  `passthroughSourcePort` walks each DAC input back and accepts only transparent
  blocks in between - `dbg.meter` passes its value through untouched, so a
  `ADC -> meters -> DAC` chain still qualifies - then requires the ADC to be
  Unity, its Audio input to be `src.audio`, and the channel count to match. The
  check is deliberately **structural**: it never looks at consumers, because a
  Spectrum Analyzer hanging off the DAC must keep reading the rendered graph
  (which always runs live, even when the muxing is skipped). The only measured
  condition is the conservative clamp guard (`MediaRef::peak <= 1`, derived and
  re-measured on import and project open because it is not saved). Dropping any
  condition silently changes the exported audio.
- Monitor playback follows the route live: `prepareMonitorAudio` starts
  `AudioClip::startLiveStream` for a processed chain and the app pushes the DAC
  window after every preview render, so rewiring the Audio Output or editing a
  parameter is heard on the next displayed frame. `Exporter::renderOutputAudio`
  remains the offline per-frame renderer used by export.
- A node's rendered audio buffer is only valid for the pass that filled it.
  `Exporter::resetRenderedAudio` is called at the end of the audio pre-pass, at
  the end of a run and when the monitor starts a processed stream: an export
  leaves whole-track buffers whose windows the preview's reuse check would accept
  as fresh, and the monitor would then never be handed a new sample again (the
  app went silent after an export).
- The editor follows an export in its own indicators: `performExport` sets
  `state.playhead` from `ExportProgress::videoTime` (before the redraw throttle,
  so the bars show the frame actually rendering) and restores the previous
  playhead afterwards. Keep the closing `ExportProgress` call reporting
  `videoTime = end`, or the bar would jump back to zero on the last callback and
  a Self Reference capture records that glitch.
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
- The VU meter integrates the rectified signal with time-constant ballistics
  (`dtOf(ctx)` based, so it reads the same at 24 fps, 240 fps and inside an
  audio-rate region): ~100 ms rising and 0.3-1.2 s falling through the Release
  knob, with 0 VU at -18 dBFS. The Digital mode must stay raw - no averaging,
  no release - because that is what its diagram is for; it also keeps the
  history push at video rate inside a region.
- Dynamics applies pre-gain and post-gain outside the compressor's ballistics,
  so both must ramp across a rendered window (`DynamicsSettings::rampGains` with
  the `...DbStart` values `evalDynamics` keeps in `runtimeState`) whenever the
  window continues the previous one. A frame-rate modulation or slider move is
  otherwise a per-window gain step, which is audible as cracking. Threshold and
  ratio need no ramp because the detector/gain followers already smooth them.
- Resolution-modulating blocks must not reallocate targets per frame: Spectrum
  quantises the modulated scale and keeps its feedback texture at the base
  scale, and `Renderer::acquire` caps the target pool at 12 with LRU eviction.
- `fx.transform` reads its Matrix port as a 2D affine map: `uMatrix[row][column]`
  is exactly the row-major grid the Matrix block's Inspector shows, the top-left
  2x2 is the linear part and the translation is rows 0/1 of the 3rd column plus
  the 4th (so a 3x3 homogeneous grid and a 4x4 OpenGL-style grid both
  translate). Amount blends the matrix towards the identity and the scalars are
  a pre-offset applied before the matrix acts, which is what lets a rotation or
  scale pivot anywhere. The inverse mapping in the `transform` builtin and the
  README table describe the same layout - change both together.
- `assets/shaders` ships copy-me templates (`passthrough.glsl`, `scale.glsl`,
  `rotation.glsl`) beside the preset shaders; the selftest compiles all three
  through the Shader block, so a preamble change that breaks them fails fast.
- The **Picture** and **Textbox** blocks keep their GPU resources in the
  Renderer, not in core: `Renderer::picture` caches one texture per frame per
  block id (reloaded when the path changes, unloaded in `shutdown`), and
  `Renderer::textFont` caches custom .ttf atlases. The editor hands over the
  TTFs its own atlases are built from with `setDefaultFonts`, so a Textbox can
  rasterise at the size the video needs instead of upscaling a fixed atlas
  (CrystalGUI's font by default). Stills load through raylib first and ffmpeg is
  the fallback; `ffmpeg::decodeImageSequence` scales to the cap *before*
  decoding and bounds frames and bytes, because `-f rawvideo` is uncompressed -
  never feed it an unbounded image. Animated frames are picked with
  `ctx.time * file fps`, so preview and export show the same frame. The Textbox
  wraps text with its own small core-side wrapper (the UI one is not reachable
  from core) and always draws at least the first line, so a region shorter than
  one line still shows something.
- **Self Reference** (`render.selfref`) hands out `Renderer::screenCapture()`,
  the framebuffer the editor is currently drawing into. Keep the three
  properties that make it usable:
  *lazy*: `captureEditorFrame` (App.cpp) checks `graphNeedsScreenCapture` first
  and runs at most once per **video** frame (`UiState::lastCaptureFrame`), so the
  mirror moves at the project's frame rate instead of jittering with the display
  refresh; a graph without the block pays nothing and the export keeps its
  ~12 Hz progress redraw (when a mirror *is* present the export redraws every
  frame, which is what keeps the exported mirror smooth);
  *cheap*: `refreshScreenCapture` reuses one texture and blits framebuffer to
  framebuffer (no CPU readback), flipping the rows so v = 0 is the visual top
  like every other target, and it restores raylib's tracked FBO bindings;
  *stable*: the capture is taken **after** `drawEditor` and **before** the export
  dim/progress overlay, so the grey-out never enters the pipeline, and the block
  therefore trails the graph by one frame - that lag is what prevents the mirror
  from feeding back into itself, so do not "fix" it by capturing before the
  graph runs. During an export the preview pane is fed the frame the exporter
  just rendered, so the editor (and its mirror) shows the pipeline live.
- Geometry (`geom.primitives`) owns primitives only and must not read
  `ctx.audio`/`ctx.analysis`. Spectrum and waveform shapes are
  `render.spectrum` effects; `Project::fromJson` converts old Geometry shapes
  3..6 (Radial Bars, Bar Spectrum, Waveform Ring, Waveform Line) to the
  Spectrum element presets `radial_bars`/`bar_spectrum`/`waveform_ring`/
  `waveform_line`, which run the original `geometry::drawSpectrumElement` code
  and keep the old count/radius/thickness/spin/colour parameters. The migration
  remaps the old Scale input to the Spectrum Scale port and wires the project
  analyzer to the Analysis input. `remapGeometryShape` shifts the remaining
  primitive enum values (old 7..10 -> new 3..6). Keep `pf_migrate` in step, and
  never reintroduce Analysis/Audio inputs or global-analysis use in Geometry.
- Spectrum's preset decides the block's schema: `Registry::spectrumSchemaFor`
  builds the ports and Inspector parameters (shader presets add their own
  `uUser[i]` modulation ports, the Geometry elements add Rotation/X/Y/Position
  and shape parameters), and `Registry::applySpectrumPreset` rebuilds a node
  when the preset changes, preserving values of keys the new preset also has.
  `Graph::addNodeWithId` and `Project::fromJson` apply it on creation/load, and
  `Graph::evaluate` re-syncs it through `runtimeState["spectrum.preset"]`.
  Because links are index-based, the sync re-matches the node's input links by
  port name and drops the ones whose port is gone. Append new presets at the
  end of `Registry::spectrumPresetNames()` (shader effect indices must not
  shift), and make every per-preset shader parameter actually read its
  `uUser[slot]` (`pfParam(slot, fallback)` for positive-only values).
- The Spectrum waveform display maps +/-40 ms around the playhead across the
  whole bar. A whole-file source is sampled directly, but a live analysis (the
  analyzer of a processed chain) only carries one video frame of source at a
  time, so `Node::pushWaveHistory` accumulates the last ~0.12 s in the block's
  rolling `waveHistory` and the display maps that instead. Without it the
  preview showed a short window stretched (and, before the buffer was clipped,
  clamped to a flat line at both ends) while the export - whose rendered buffer
  covers the whole track - showed the full span. Keep the two paths producing
  the same window, and reset the ring when the incoming window does not
  continue it (a seek or a rewired source).
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
  the samples the region produced in the same evaluation. A processed input
  reaches this block one video frame at a time, which is shorter than the FFT
  window, so the analyzer first pushes what it receives into the node's
  `waveHistory` and analyses the last `fftSize` samples of that history: without
  it the preview zero-padded most of the window and a Frequency Band modulation
  read differently from the export, which sees the whole rendered buffer.
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
  blocks several times per project frame. A frame-rate window producer that
  feeds the region (Dynamics) quantises the same way, otherwise every display
  tick restarts its buffer at a slightly later `startFrame` and the ADC reads
  past the end of it - which is audible as periodic zero-fills even though the
  export is clean. `pushLiveAudioWindow` streams the window of whichever node
  drives the Audio Output (DAC or Dynamics), matching the exporter's sink walk.
  Export still renders the same per-frame pass offline and muxes the resulting
  track.
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
