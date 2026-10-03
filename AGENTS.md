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
  `pf_probe.exe`, `pf_stage.exe`). The app expects `resource/` and `assets/`
  next to the executable, or beside the working directory.
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
audio matching rules. When NVENC is usable it also renders a real GPU export.

It writes `selftest_output.mp4`, `selftest_project.pforge`,
`selftest_legacy.pforge` and (when GPUs are available) `selftest_nvenc.mp4` into
the current directory - delete them when you are done.

Other tools:

- `pf_stage.exe <audio> <stage> <out.png> [time] [width] [height]` - renders one
  pipeline stage (`--help` is not implemented; the stage numbers are documented
  at the top of `tools/stage_dump.cpp`).
- `pf_probe.exe` - OpenGL/readback probe for render-target experiments.

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
