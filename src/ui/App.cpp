// Application shell: window setup, CrystalGUI chrome, layout and main loop.
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <unordered_map>
#include <string>

#include "core/Registry.h"
#include "dsp/Analysis.h"
#include "export/Exporter.h"
#include "export/FFmpeg.h"
#include "render/Palette.h"
#include "render/FrameReadback.h"
#include "ui/App.h"

namespace pf {

// The scrollable part of the palette panel, shared by the chrome clipping and
// the caption drawing so they always agree.
Rectangle paletteView(Rectangle bounds);

namespace {

UiState *gApp = nullptr;
CguiNode *gPaletteLayer = nullptr;
CguiNode *gTopLayer = nullptr;
// Captions are drawn by the app (CrystalGUI labels are pointer-based and its
// linear layout is not precise enough for dense lists), so the positions are
// cached here when the chrome is built.
std::vector<std::pair<std::string, float>> gPaletteCaptions;
// CrystalGUI's absolute transformation is in screen coordinates, so the placed
// rect of every chrome element is cached at build time.
struct ChromeRect {
    CguiNode *node = nullptr;
    Rectangle rect{};
    std::string label;
    CguiButtonPressCallback action = nullptr;
    bool accent = false;
    bool flat = false;
    bool topBar = false;
    float localY = 0.0f;  // palette entries scroll, so the y is recomputed per frame
};
std::vector<ChromeRect> gChromeRects;
// Index of the chrome entry holding the mouse button, -1 when none is held.
int gChromePressedIndex = -1;
// CrystalGUI resets instance node names when they are synced from the template,
// so the block kind is tracked next to the node instead of in its name.
std::unordered_map<const CguiNode *, std::string> gPaletteKinds;
float gPaletteRowHeight = 24.0f;
float gPaletteContentHeight = 0.0f;
float gPaletteScroll = 0.0f;
float gTopBarContentWidth = 0.0f;

constexpr const char *kDefaultProject = "demo.pforge";

// ---------------------------------------------------------------------------
// CrystalGUI callbacks
// ---------------------------------------------------------------------------

void actionNew(CguiNode *) { if (gApp) newProject(*gApp); }

void actionOpen(CguiNode *) {
    if (!gApp) return;
    openBrowser(*gApp, "project-open", "Open project", ".pforge", gApp->projectDirectory);
}

void actionSave(CguiNode *) {
    if (!gApp) return;
    if (gApp->project.filePath.empty()) {
        openBrowser(*gApp, "project-save", "Save project", ".pforge", gApp->projectDirectory);
    } else {
        saveProjectFile(*gApp, gApp->project.filePath);
    }
}

void actionExport(CguiNode *) {
    if (!gApp) return;
    gApp->showExportDialog = true;
    updateExportExtension(*gApp);
}

void actionPlay(CguiNode *) {
    if (!gApp) return;
    gApp->playing = !gApp->playing;
    if (gApp->playing) {
        startPlayback(*gApp);
    } else {
        pausePlayback(*gApp);
    }
}

void actionStop(CguiNode *) {
    if (!gApp) return;
    stopPlayback(*gApp);
}

void actionTheme(CguiNode *) { ui::setDarkTheme(!ui::isDarkTheme()); }

void actionLoop(CguiNode *) {
    if (gApp) gApp->loopPlayback = !gApp->loopPlayback;
}

void actionHelp(CguiNode *) {
    if (gApp) gApp->showHelp = !gApp->showHelp;
}

void actionPreferences(CguiNode *) {
    if (gApp) gApp->showPreferences = !gApp->showPreferences;
}

void actionPaletteEntry(CguiNode *node) {
    if (!gApp || !node) return;
    const auto it = gPaletteKinds.find(node);
    if (it == gPaletteKinds.end()) {
        setStatus(*gApp, "Palette button has no block kind bound", true);
        return;
    }
    addNodeFromKind(*gApp, it->second);
}

// ---------------------------------------------------------------------------
// Chrome construction
// ---------------------------------------------------------------------------


struct TopButtonSpec {
    const char *label;
    CguiButtonPressCallback action;
    int type;
    float width;
};

void buildChrome(UiState &state) {
    if (state.root) {
        CguiDeleteNode(state.root);
        state.root = nullptr;
    }
    gPaletteCaptions.clear();
    gChromeRects.clear();
    gPaletteKinds.clear();
    gChromePressedIndex = -1;
    CguiNode *root = CguiCreateRoot();
    state.root = root;

    // ---- top bar ---------------------------------------------------------
    // Plain nodes rather than layers: the PulseForge panels draw their own
    // backgrounds, and a themed layer would paint over them.
    gTopLayer = CguiCreateNodeEx(CguiTAbsolute(Vector2{0, 0}, Vector2{10, 10}), "topbar");
    CguiInsertChild(root, gTopLayer);
    // Explicit positions keep every button the same size and baseline, which a
    // linear layout with variable label widths did not.
    const TopButtonSpec topButtons[] = {
        {"New", actionNew, CGUI_BUTTON_TYPE_NORMAL, 54.0f},
        {"Open", actionOpen, CGUI_BUTTON_TYPE_NORMAL, 60.0f},
        {"Save", actionSave, CGUI_BUTTON_TYPE_NORMAL, 60.0f},
        {"Export", actionExport, CGUI_BUTTON_TYPE_ACCENT, 72.0f},
        {"Play", actionPlay, CGUI_BUTTON_TYPE_FLAT, 56.0f},
        {"Stop", actionStop, CGUI_BUTTON_TYPE_FLAT, 56.0f},
        {"Loop", actionLoop, CGUI_BUTTON_TYPE_FLAT, 58.0f},
        {"Theme", actionTheme, CGUI_BUTTON_TYPE_FLAT, 64.0f},
        {"Prefs", actionPreferences, CGUI_BUTTON_TYPE_FLAT, 58.0f},
        {"Help", actionHelp, CGUI_BUTTON_TYPE_FLAT, 54.0f},
    };
    float x = ui::s(8.0f);
    for (const TopButtonSpec &spec : topButtons) {
        const Rectangle rect{x, ui::s(6.0f), ui::s(spec.width), ui::s(26.0f)};
        CguiNode *buttonNode = CguiCreateButton(
            CguiTAbsolute(Vector2{rect.x, rect.y}, Vector2{rect.width, rect.height}), spec.type,
            spec.action, false);
        if (!buttonNode) continue;
        // No label child: CrystalGUI's label component renders glyphs
        // incorrectly in this build, so the caption is drawn by the app on top
        // of the themed button box.
        ChromeRect entry;
        entry.node = buttonNode;
        entry.rect = rect;
        entry.label = spec.label;
        entry.action = spec.action;
        entry.accent = spec.type == CGUI_BUTTON_TYPE_ACCENT;
        entry.flat = spec.type == CGUI_BUTTON_TYPE_FLAT;
        entry.topBar = true;
        gChromeRects.push_back(entry);
        CguiInsertChild(gTopLayer, buttonNode);
        x += rect.width + ui::s(6.0f);
    }
    gTopBarContentWidth = x + ui::s(8.0f);

    // ---- palette ---------------------------------------------------------
    gPaletteLayer = CguiCreateNodeEx(CguiTAbsolute(Vector2{0, 0}, Vector2{10, 10}), "palette");
    CguiInsertChild(root, gPaletteLayer);

    // The labels store the pointer they are given, so the strings must outlive
    // the node tree.
    static const std::vector<std::string> categories = Registry::instance().categories();
    int buttonCount = 0;
    for (const std::string &category : categories) {
        buttonCount += static_cast<int>(Registry::instance().byCategory(category).size());
    }
    const float available = std::max(ui::s(120.0f), state.paletteRect.height - ui::s(40.0f));
    const float captionBudget = static_cast<float>(categories.size()) * ui::s(22.0f);
    gPaletteRowHeight = std::clamp((available - captionBudget) / std::max(1, buttonCount),
                                   ui::s(18.0f), ui::s(26.0f));

    float y = 0.0f;
    const float originX = state.paletteRect.x + ui::s(6.0f);
    const float originY = state.paletteRect.y + ui::s(30.0f);
    const float paletteWidth = std::max(ui::s(80.0f), state.paletteRect.width - ui::s(14.0f));
    for (const std::string &category : categories) {
        gPaletteCaptions.emplace_back(category, y);
        y += ui::s(22.0f);
        for (const NodeDef *def : Registry::instance().byCategory(category)) {
            // The press callback reads the block kind from the node name.
            const Rectangle rect{originX, originY + y, paletteWidth, gPaletteRowHeight};
            CguiNode *buttonNode = CguiCreateButton(
                CguiTAbsolute(Vector2{rect.x, rect.y}, Vector2{rect.width, rect.height}),
                CGUI_BUTTON_TYPE_NORMAL, actionPaletteEntry, false);
            if (!buttonNode) continue;
            gPaletteKinds[buttonNode] = def->kind;
            ChromeRect entry;
            entry.node = buttonNode;
            entry.rect = rect;
            entry.label = def->label;
            entry.action = actionPaletteEntry;
            entry.topBar = false;
            entry.localY = y;
            gChromeRects.push_back(entry);
            CguiInsertChild(gPaletteLayer, buttonNode);
            y += gPaletteRowHeight + ui::s(3.0f);
        }
        y += ui::s(6.0f);
    }
    gPaletteContentHeight = y;
    gPaletteScroll = 0.0f;
}

// The chrome buttons are painted by CrystalGUI but clicked through this
// function. CrystalGUI's own dispatch cannot be trusted for them: it routes a
// press to whatever node it hovered first, which showed up as "Save" adding an
// Audio Source block. The nodes are therefore visual only
// (canHandleMouseEvents is cleared every frame) and the CrystalGUI hover/held
// state is written from the hit test below so the themed boxes keep their
// hover and pressed colours.
void updateChromeInput(UiState &state, Vector2 mouse, bool pressed, bool released) {
    const bool interactive = !ui::inputBlocked();
    const Rectangle paletteClip = paletteView(state.paletteRect);
    for (size_t i = 0; i < gChromeRects.size(); ++i) {
        ChromeRect &entry = gChromeRects[i];
        if (!entry.node) continue;
        const bool visible = entry.topBar || CheckCollisionPointRec(mouse, paletteClip);
        const bool hovered =
            interactive && visible && CheckCollisionPointRec(mouse, entry.rect);
        if (interactive && pressed && hovered) {
            gChromePressedIndex = static_cast<int>(i);
        }
        const bool held = static_cast<int>(i) == gChromePressedIndex;
        if (auto *iData = static_cast<CguiButtonInstanceData *>(entry.node->instanceData)) {
            iData->hovered = hovered;
            iData->held = hovered && held;
            // Only the export greys the chrome out. A popup or dialog blocks
            // clicks on its own, and greying there made the whole top bar fade
            // away whenever a dropdown list was open.
            iData->disabled = state.exporting;
        }
        if (released && held) {
            gChromePressedIndex = -1;
            if (hovered && entry.action) entry.action(entry.node);
        }
    }
    if (!interactive || released) gChromePressedIndex = -1;
}

void updateChromeLayout(UiState &state) {
    if (gTopLayer) {
        gTopLayer->transformation =
            CguiTAbsolute(Vector2{state.topBarRect.x, state.topBarRect.y},
                          Vector2{state.topBarRect.width, state.topBarRect.height});
        gTopLayer->rebound = true;
    }
    if (gPaletteLayer) {
        // The palette node sits just under the panel header; the buttons and
        // captions are positioned inside it in local coordinates.
        gPaletteLayer->transformation =
            CguiTAbsolute(Vector2{state.paletteRect.x, state.paletteRect.y + 30.0f},
                          Vector2{state.paletteRect.width - 12.0f,
                                  state.paletteRect.height - 38.0f});
        gPaletteLayer->rebound = true;
        // Buttons are placed in screen space, so scrolling is applied here.
        const float originX = state.paletteRect.x + ui::s(6.0f);
        const float originY = state.paletteRect.y + ui::s(30.0f) - gPaletteScroll;
        for (ChromeRect &entry : gChromeRects) {
            if (entry.topBar || !entry.node) continue;
            entry.rect.x = originX;
            entry.rect.y = originY + entry.localY;
            entry.node->transformation = CguiTAbsolute(Vector2{entry.rect.x, entry.rect.y},
                                                       Vector2{entry.rect.width, entry.rect.height});
            entry.node->rebound = true;
        }
    }
    for (const ChromeRect &entry : gChromeRects) {
        if (entry.node) entry.node->canHandleMouseEvents = false;
    }
    updateChromeInput(state, GetMousePosition(), IsMouseButtonPressed(MOUSE_BUTTON_LEFT),
                      IsMouseButtonReleased(MOUSE_BUTTON_LEFT));
}

}  // namespace

// ---------------------------------------------------------------------------
// Status helpers
// ---------------------------------------------------------------------------

void setStatus(UiState &state, const std::string &message, bool error) {
    state.statusMessage = message;
    state.statusUntil = GetTime() + 5.0;
    state.statusIsError = error;
    // Mirrored to the log so automated runs can assert on UI actions.
    TraceLog(error ? LOG_WARNING : LOG_INFO, "PulseForge: %s", message.c_str());
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

namespace {

// Applies the match to the project and, when required, transcodes the decoded
// clip to AAC in memory. Returns a short description for the status line.
std::string applyImportedAudio(UiState &state, bool allowTranscode) {
    Project &project = state.project;
    if (project.audio.codec.empty() && !state.clip.valid()) return std::string();
    const int sourceKbps = static_cast<int>(project.audio.bitRate / 1000);
    const AudioEncoderChoice match =
        chooseAudioEncoder(project.audio.codec, sourceKbps, project.output.container,
                           [](const std::string &id) { return ffmpeg::hasEncoder(id); });
    if (allowTranscode) {
        state.clip.clearTranscodedAudio();
        project.audio.transcodedAac = false;
    }
    if (!match.encoder.empty()) project.output.audioCodec = match.encoder;
    if (match.bitrateKbps > 0) {
        project.output.audioBitrate = std::to_string(match.bitrateKbps) + "k";
    }

    std::string note = audioCodecDisplayName(project.output.audioCodec);
    if (!project.output.audioBitrate.empty() &&
        !audioCodecIsLossless(project.output.audioCodec)) {
        note += " " + project.output.audioBitrate;
    }
    if (match.transcodeToAac && allowTranscode && state.clip.valid() &&
        !state.clip.hasTranscodedAudio()) {
        const AudioPtr &buffer = state.clip.buffer();
        std::vector<unsigned char> bytes;
        std::string error;
        const double started = GetTime();
        const int outputRate =
            project.output.audioSampleRate > 0 ? project.output.audioSampleRate
                                               : buffer->sampleRate;
        if (ffmpeg::encodeAacInMemory(buffer->samples.data(), buffer->frameCount, buffer->channels,
                                      buffer->sampleRate, outputRate, match.bitrateKbps, &bytes,
                                      &error)) {
            state.clip.setTranscodedAudio(std::move(bytes));
            project.audio.transcodedAac = true;
            project.audio.transcodedRate = outputRate;
            char suffix[64];
            std::snprintf(suffix, sizeof(suffix), " (converted in memory, %.1fs)",
                          GetTime() - started);
            note += suffix;
        } else {
            note += " (AAC conversion failed)";
            setStatus(state, "Could not convert the audio to AAC: " + error, true);
        }
    } else if (project.audio.transcodedAac) {
        note += " (in memory)";
    }
    return note;
}

// Cheap identity of the Audio Output wiring, used to notice a rewire while the
// transport is running. Parameter edits are picked up on the next Play.
std::string monitorRouteIdentity(const UiState &state) {
    const Project &project = state.project;
    const int sink = project.graph.audioSinkNodeId();
    if (sink == 0) return "none";
    const Link *link = project.graph.findInputLink(sink, 0);
    if (!link) return "open";
    const Node *source = project.graph.find(link->fromNode);
    if (!source || !source->enabled) return "open";
    char text[320];
    const size_t channels =
        source->kind == "dsp.dac" ? source->inputPorts().size() : 0;
    std::snprintf(text, sizeof(text), "%d:%s|%zu|%s|%.4f|%.4f", source->id,
                  source->kind.c_str(), channels, project.audio.path.c_str(),
                  project.video.trimStart, project.effectiveDuration(project.audio.duration));
    return text;
}

// Full fingerprint of everything the rendered track depends on; the view is
// excluded so panning and the playhead do not invalidate it.
std::string monitorRenderKey(const UiState &state) {
    Project copy = state.project;
    copy.view = ViewState{};
    // Moving blocks around does not change the rendered track.
    for (Node &node : copy.graph.nodes) {
        node.x = 0.0f;
        node.y = 0.0f;
        node.title.clear();
    }
    const std::string document = json::write(copy.toJson(state.projectDirectory), 0);
    unsigned long long hash = 1469598103934665603ull;
    for (unsigned char c : document) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    char text[32];
    std::snprintf(text, sizeof(text), "%016llx", hash);
    return text;
}

// Applies the current Audio Output route to the monitor: the decoded clip for a
// direct source link, a live graph-rendered stream for a processed chain,
// silence for an unconnected output.
bool prepareMonitorAudio(UiState &state, std::string *error) {
    (void)error;
    state.clip.clearPlaybackBuffer();
    switch (Exporter::audioRoute(state.project)) {
        case AudioRoute::Source:
            state.clip.stopLiveStream();
            return true;
        case AudioRoute::Silent:
            state.clip.stopLiveStream();
            state.clip.setPlaybackBuffer(nullptr);
            return true;
        case AudioRoute::Processed: {
            // Render the chain live, one video frame at a time, so the audio
            // follows the video clock and parameter edits are heard at once.
            // A previous export can leave whole-track buffers whose windows the
            // preview would reuse instead of rendering new ones, so drop them
            // before the stream starts.
            Exporter::resetRenderedAudio(state.project.graph);
            const int rate = state.clip.sampleRate() > 0 ? state.clip.sampleRate() : 48000;
            int channels = 2;
            const int sink = state.project.graph.audioSinkNodeId();
            const Link *link = sink ? state.project.graph.findInputLink(sink, 0) : nullptr;
            const Node *source = link ? state.project.graph.find(link->fromNode) : nullptr;
            if (source && source->kind == "dsp.dac") {
                channels = std::clamp(static_cast<int>(source->inputPorts().size()), 1, 8);
            }
            const double fps = std::max(1.0, state.project.video.fps);
            const int frameSamples =
                std::max(1, static_cast<int>(std::lround(rate / fps)));
            state.clip.stopLiveStream();
            state.clip.startLiveStream(rate, channels, frameSamples);
            return true;
        }
    }
    return true;
}

// The Self Reference block reads the editor's own window. Capturing is not free
// (a full-window GPU blit), so it only happens while one of these blocks is
// actually enabled in the graph - a project that does not use it pays nothing.
bool graphNeedsScreenCapture(const Graph &graph) {
    for (const Node &node : graph.nodes) {
        if (node.enabled && node.kind == "render.selfref") return true;
    }
    return false;
}

// Refreshes the Self Reference texture from the framebuffer currently being
// drawn into, before the export overlay covers it. It follows the *video* frame
// counter rather than the display refresh, so every rendered frame sees one
// stable screenshot and the mirror moves at the project's frame rate instead of
// jittering with the monitor.
void captureEditorFrame(UiState &state, int videoFrame) {
    if (state.lastCaptureFrame == videoFrame) return;
    if (!graphNeedsScreenCapture(state.project.graph)) return;
    state.renderer.refreshScreenCapture();
    state.lastCaptureFrame = videoFrame;
}

void resetMonitor(UiState &state) {
    state.clip.stopLiveStream();
    state.clip.clearPlaybackBuffer();
    state.monitorRenderKey.clear();
    state.monitorRoute.clear();
    state.liveAudioFrame = -1;
}

// Pushes the audio window produced by the most recent graph evaluation into
// the live monitor stream. The Audio Output may be fed by a DAC or by another
// Audio-producing block (Dynamics, ...), so the window comes from whichever
// node drives the sink - the same node the exporter muxes.
void pushLiveAudioWindow(UiState &state) {
    if (!state.clip.liveStreamActive()) return;
    const int sink = state.project.graph.audioSinkNodeId();
    const Link *link = sink ? state.project.graph.findInputLink(sink, 0) : nullptr;
    const Node *source = link ? state.project.graph.find(link->fromNode) : nullptr;
    if (source && source->audioRenderOutput) {
        state.clip.pushLiveWindow(*source->audioRenderOutput);
    }
}

// Renders the project frames the display skipped, in order, so the live audio
// stream stays continuous when the display runs below the project frame rate.
void catchUpLiveAudio(UiState &state, int targetFrame) {
    if (!state.clip.liveStreamActive()) return;
    const double fps = std::max(1.0, state.project.video.fps);
    if (state.liveAudioFrame < 0 || targetFrame < state.liveAudioFrame ||
        targetFrame - state.liveAudioFrame > 8) {
        // Seek, loop or a large jump: restart at the new frame.
        state.liveAudioFrame = targetFrame;
        return;
    }
    while (state.liveAudioFrame < targetFrame) {
        EvalContext ctx = state.frameContext;
        ctx.renderer = nullptr;
        ctx.shaders = nullptr;
        ctx.offline = false;
        ctx.frame = state.liveAudioFrame;
        ctx.time = static_cast<double>(ctx.frame) / fps;
        ctx.audioTime = state.project.video.trimStart + ctx.time;
        state.project.graph.evaluate(ctx);
        pushLiveAudioWindow(state);
        ++state.liveAudioFrame;
    }
}

}  // namespace

void refreshOutputAudio(UiState &state) {
    if (state.project.audio.path.empty() && !state.clip.valid()) return;
    applyImportedAudio(state, false);
}

void refreshShaderPorts(UiState &state, Node &node, bool force) {
    if (node.kind != "render.shader") return;
    const std::string path = node.pstr("shader");
    const auto it = state.shaderPortKey.find(node.id);
    if (!force && it != state.shaderPortKey.end() && it->second == path) return;
    state.shaderPortKey[node.id] = path;
    std::string error;
    if (!Registry::applyShaderPorts(node, state.renderer.shaders(), &error)) {
        setStatus(state, "Shader inputs: " + error, true);
    }
}

void updateExportExtension(UiState &state) {
    state.exportPath = exportPathForContainer(state.exportPath, state.project.output.container,
                                              state.projectDirectory);
}

void startPlayback(UiState &state) {
    std::string error;
    if (!prepareMonitorAudio(state, &error)) {
        state.playing = false;
        setStatus(state, "Playback: " + error, true);
        return;
    }
    state.monitorRenderKey = monitorRenderKey(state);
    state.monitorRoute = monitorRouteIdentity(state);
    state.liveAudioFrame = static_cast<int>(std::floor(
        std::max(0.0, state.playhead) * std::max(1.0, state.project.video.fps)));
    state.playing = true;
    state.clip.seek(state.playhead + state.project.video.trimStart);
    state.clip.startPreview();
}

void pausePlayback(UiState &state) {
    state.playing = false;
    state.clip.pausePreview();
}

void stopPlayback(UiState &state) {
    state.playing = false;
    state.playhead = 0.0;
    state.liveAudioFrame = -1;
    state.clip.stopPreview();
    state.clip.seek(state.project.video.trimStart);
}

void newProject(UiState &state) {
    state.project.resetToDefault(&state.renderer.shaders());
    selectNode(state, state.project.graph.videoSinkNodeId());
    state.playhead = 0.0;
    state.playing = false;
    state.clip.stopPreview();
    resetMonitor(state);
    state.analysis.reset();
    if (state.project.audio.path.empty()) state.clip.clear();
    state.shaderPortKey.clear();
    updateExportExtension(state);  // the container went back to the default
    setStatus(state, "New project created");
}

void loadAudioFile(UiState &state, const std::string &path) {
    // Guard rails: material this large cannot be analysed and previewed in real
    // time, so it is rejected up front with an explanation.
    constexpr double kMaxMegabytes = 100.0;
    constexpr int kMaxSampleRate = 192000;
    std::error_code sizeError;
    const auto fileSize = std::filesystem::file_size(path, sizeError);
    if (!sizeError) {
        const double megabytes = static_cast<double>(fileSize) / (1024.0 * 1024.0);
        if (megabytes > kMaxMegabytes) {
            char text[320];
            std::snprintf(text, sizeof(text),
                          "%s is %.1f MB.\n\nThe limit is %.0f MB: larger files cannot be "
                          "decoded, analysed and previewed in real time.",
                          std::filesystem::path(path).filename().string().c_str(), megabytes,
                          kMaxMegabytes);
            showMessage(state, "File too large", text, true);
            return;
        }
    }
    const MediaInfo probe = ffmpeg::probe(path);
    if (probe.ok && probe.sampleRate > kMaxSampleRate) {
        char text[320];
        std::snprintf(text, sizeof(text),
                      "%s is %d Hz.\n\nThe limit is %d Hz: higher sample rates cannot be "
                      "analysed and previewed in real time. Resample it first, for example:\n"
                      "ffmpeg -i in.flac -ar 48000 out.flac",
                      std::filesystem::path(path).filename().string().c_str(), probe.sampleRate,
                      kMaxSampleRate);
        showMessage(state, "Sample rate too high", text, true);
        return;
    }

    // The clip is decoded at the rate it was recorded at, so nothing is
    // resampled on import and the analysis sees the original spectrum. Only the
    // output rate is fixed by the project (48 kHz by default).
    const int decodeRate = std::clamp(probe.ok && probe.sampleRate > 0 ? probe.sampleRate : 48000,
                                      8000, kMaxSampleRate);
    std::string error;
    if (!state.clip.load(path, decodeRate, &error)) {
        showMessage(state, "Could not load audio", error, true);
        return;
    }
    state.playing = false;
    resetMonitor(state);
    state.project.audio.path = path;
    state.project.audio.duration = state.clip.duration();
    state.project.audio.sampleRate = state.clip.sampleRate();
    state.project.audio.channels = state.clip.channels();
    state.project.audio.codec = probe.codec;
    state.project.audio.bitRate = probe.bitRate;
    state.project.audio.transcodedAac = false;
    state.project.audio.fileSize = static_cast<long long>(fileSize);
    // Peak of the decoded clip: the passthrough ADC -> DAC check needs it to
    // know whether the DAC's clamp could change a sample.
    state.project.audio.peak = 0.0f;
    if (const AudioPtr &buffer = state.clip.buffer()) {
        float peak = 0.0f;
        for (const float sample : buffer->samples) peak = std::max(peak, std::fabs(sample));
        state.project.audio.peak = peak;
    }

    state.clip.buildOverview();
    state.analysis = analyzeAudio(*state.clip.buffer(), AnalysisSettings{}, {}, state.clip.buffer());
    // Follow the imported media: same codec family and bitrate when this ffmpeg
    // build can carry it, otherwise a one-off AAC conversion held in memory.
    const std::string audioNote = applyImportedAudio(state, true);
    state.playhead = 0.0;
    state.project.dirty = true;
    char message[320];
    std::snprintf(message, sizeof(message), "Loaded %.2f s of audio (%d Hz, %d analysis frames)%s%s",
                  state.clip.duration(), state.clip.sampleRate(),
                  state.analysis ? static_cast<int>(state.analysis->frames.size()) : 0,
                  audioNote.empty() ? "" : "  -  export audio ", audioNote.c_str());
    setStatus(state, message);
}

void selectNode(UiState &state, int nodeId) {
    if (state.selectedNode == nodeId) return;
    ui::cancelEdits();
    state.selectedNode = nodeId;
    state.project.view.selectedNode = nodeId;
    state.inspectorScroll = 0.0f;
}

void handleDroppedFile(UiState &state, const std::string &path) {
    std::string extension = std::filesystem::path(path).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension == ".pforge") {
        loadProjectFile(state, path);
        return;
    }
    if (extension == ".png" || extension == ".jpg" || extension == ".jpeg") {
        showMessage(state, "Unsupported drop",
                    "Images are not imported yet - drop an audio file or a .pforge project.", true);
        return;
    }
    loadAudioFile(state, path);
}

void updateWindowTitle(UiState &state) {
    const std::string title =
        state.project.name + (state.project.dirty ? " *" : "") + "  -  PulseForge";
    if (title != state.windowTitle) {
        state.windowTitle = title;
        SetWindowTitle(title.c_str());
        TraceLog(LOG_INFO, "PulseForge: window title \"%s\"", title.c_str());
    }
}

// Draws the whole editor. `interactive` is false while a modal owns the input
// (or while exporting), in which case the panels are drawn but frozen and the
// dialogs are handled by the caller.
void drawChromeLabels(UiState &state);
void drawPaletteLabels(UiState &state, Rectangle bounds);

void drawEditor(UiState &state, bool interactive) {
    // Two modal layers: the editor behind is frozen and ignores the mouse, while
    // the dialog drawn on top stays interactive on its own account.
    ui::setModal(!interactive);
    drawPalette(state, state.paletteRect);
    drawGraphCanvas(state, state.canvasRect);
    drawTimeline(state, state.timelineRect);
    drawPreview(state, state.previewRect);
    drawInspector(state, state.inspectorRect);
    drawTopBar(state, state.topBarRect);
    drawStatusBar(state, state.statusRect);

    // The CrystalGUI chrome goes before the modal dim overlays so dialogs dim
    // the buttons exactly like the rest of the editor.
    if (state.root) {
        if (interactive) CguiUpdate(state.root);
        // The palette scrolls, so its CrystalGUI buttons are clipped to the
        // panel: otherwise a short window lets them draw over the status bar.
        if (gPaletteLayer) {
            const Rectangle view{paletteView(state.paletteRect)};
            BeginScissorMode(static_cast<int>(view.x), static_cast<int>(view.y),
                             static_cast<int>(view.width), static_cast<int>(view.height));
            CguiDrawNode(gPaletteLayer);
            EndScissorMode();
        }
        if (gTopLayer) CguiDrawNode(gTopLayer);
        drawChromeLabels(state);
        drawPaletteLabels(state, state.paletteRect);
    }
    // Popups are a modal of their own and are drawn whatever else is happening.
    ui::drawPopups();

    ui::setModal(false);
    drawPreferencesDialog(state);
    drawExportDialog(state);
    drawBrowser(state);
    drawHelpOverlay(state);
    drawMessageBox(state);
    updateWindowTitle(state);
}

bool modalActive(const UiState &state) {
    return state.showPreferences || state.showExportDialog || state.browser.open || state.showHelp ||
           state.showMessage || state.exporting || ui::popupOpen();
}

// True when a dialog (not a popup) owns the input.
bool dialogOpen(const UiState &state) {
    return state.showPreferences || state.showExportDialog || state.browser.open || state.showHelp ||
           state.showMessage || state.exporting;
}

void loadProjectFile(UiState &state, const std::string &path) {
    std::string error;
    Project loaded;
    if (!loaded.load(path, &error, &state.renderer.shaders())) {
        setStatus(state, "Could not open project: " + error, true);
        return;
    }
    state.project = loaded;
    state.shaderPortKey.clear();
    for (Node &node : state.project.graph.nodes) refreshShaderPorts(state, node, true);
    state.projectDirectory = Project::directoryOf(path);
    state.selectedNode = state.project.view.selectedNode;
    if (!state.project.audio.path.empty()) {
        std::string audioError;
        // Projects remember the media's own rate; fall back to the documented
        // default for projects saved before the field existed.
        const int mediaRate =
            state.project.audio.sampleRate > 0 ? state.project.audio.sampleRate : 48000;
        if (state.clip.load(state.project.audio.path, mediaRate, &audioError)) {
            state.analysis =
                analyzeAudio(*state.clip.buffer(), AnalysisSettings{}, {}, state.clip.buffer());
            // The peak is derived, so a freshly opened project measures it again.
            state.project.audio.peak = 0.0f;
            if (const AudioPtr &buffer = state.clip.buffer()) {
                float peak = 0.0f;
                for (const float sample : buffer->samples) {
                    peak = std::max(peak, std::fabs(sample));
                }
                state.project.audio.peak = peak;
            }
            // The saved output settings are the user's, so they are kept; only
            // the in-memory AAC conversion the project was saved with has to be
            // reproduced.
            if (state.project.audio.transcodedAac) applyImportedAudio(state, true);
        } else {
            setStatus(state, "Project loaded, but the audio is missing: " + audioError, true);
        }
    } else {
        state.clip.clear();
        state.analysis.reset();
    }
    state.playing = false;
    resetMonitor(state);
    state.playhead = std::clamp(state.project.view.playhead, 0.0, 1e9);
    updateExportExtension(state);  // follow the container stored in the project
    setStatus(state, "Opened " + path);
}

void saveProjectFile(UiState &state, const std::string &path) {
    state.project.view.selectedNode = state.selectedNode;
    state.project.view.playhead = state.playhead;
    std::string error;
    if (!state.project.save(path, &error)) {
        setStatus(state, "Could not save: " + error, true);
        return;
    }
    state.projectDirectory = Project::directoryOf(path);
    setStatus(state, "Saved " + path);
}

void startExport(UiState &state, const std::string &path) {
    ExportRequest request;
    request.outputPath = path;
    request.overwrite = true;
    std::string error;
    const std::vector<unsigned char> *embedded =
        state.clip.hasTranscodedAudio() ? &state.clip.transcodedAudio() : nullptr;
    const bool ok = Exporter::run(state.renderer, state.project, request, state.clip.buffer(),
                                  state.analysis, nullptr, nullptr, &error, embedded);
    if (!ok) {
        setStatus(state, "Export failed: " + error, true);
    } else {
        setStatus(state, "Exported " + path);
    }
}

// ---------------------------------------------------------------------------
// Panels owned by this file
// ---------------------------------------------------------------------------

// CrystalGUI paints the button boxes, so the captions have to be drawn after
// the chrome or the boxes would wash them out.
void drawChromeLabels(UiState &state) {
    const ui::Theme &t = ui::theme();
    for (const ChromeRect &entry : gChromeRects) {
        if (!entry.topBar) continue;
        const Color fill =
            entry.accent ? t.accent : (entry.flat ? t.controlFillFlat : t.controlFill);
        const Color ink = ui::readableOn(fill);
        ui::drawTextClipped(entry.rect, entry.label.c_str(), 13.0f, ink, ui::Align::Center, true);
    }
    (void)state;
}

void drawTopBar(UiState &state, Rectangle bounds) {
    const ui::Theme &t = ui::theme();
    DrawRectangle(static_cast<int>(bounds.x), static_cast<int>(bounds.y),
                  static_cast<int>(bounds.width), static_cast<int>(bounds.height),
                  palette::withAlpha(t.panel, 0.96f));
    DrawLine(static_cast<int>(bounds.x), static_cast<int>(bounds.y + bounds.height - 1.0f),
             static_cast<int>(bounds.x + bounds.width),
             static_cast<int>(bounds.y + bounds.height - 1.0f), t.border);

    // The CrystalGUI buttons occupy [8, gTopBarContentWidth); the rest of the bar
    // is drawn by the app, which keeps the text aligned with the buttons.
    const float left = bounds.x + gTopBarContentWidth + ui::s(8.0f);
    char title[256];
    std::snprintf(title, sizeof(title), "%s%s", state.project.name.c_str(),
                  state.project.dirty ? " *" : "");
    ui::drawTextClipped(Rectangle{left, bounds.y, ui::s(260.0f), bounds.height}, title, 15.0f, t.text,
                        ui::Align::Left, true);
    const float titleWidth = ui::textWidth(title, 15.0f, true);

    char info[256];
    std::snprintf(info, sizeof(info),
                  "%dx%d   %.0f fps   %s   audio %s   loop %s   (H = help)",
                  state.project.video.width, state.project.video.height, state.project.video.fps,
                  ffmpeg::available() ? "ffmpeg ready" : "FFMPEG MISSING",
                  state.clip.valid() ? "loaded" : "not loaded",
                  state.loopPlayback ? "on" : "off");
    ui::drawText(Rectangle{left + titleWidth + ui::s(24.0f), bounds.y, bounds.width - titleWidth - ui::s(60.0f),
                           bounds.height},
                 info, 12.0f,
                 ffmpeg::available() ? t.textDim : t.danger, ui::Align::Left);
}

void drawPalette(UiState &state, Rectangle bounds) {
    const ui::Theme &t = ui::theme();
    ui::panel(bounds, "Blocks");
    ui::drawTextClipped(Rectangle{bounds.x + ui::s(62.0f), bounds.y, bounds.width - ui::s(70.0f), ui::s(26.0f)},
                        "click to add", 10.5f, palette::withAlpha(t.textDim, 0.9f), ui::Align::Right);
    // The block list scrolls when it does not fit.
    const Rectangle view{bounds.x, bounds.y + ui::s(28.0f), bounds.width, bounds.height - ui::s(30.0f)};
    const float maxScroll = std::max(0.0f, gPaletteContentHeight - view.height);
    if (ui::hovered(view) && GetMouseWheelMove() != 0.0f) {
        gPaletteScroll = std::clamp(gPaletteScroll - GetMouseWheelMove() * ui::s(40.0f), 0.0f,
                                    maxScroll);
    }
    gPaletteScroll = std::clamp(gPaletteScroll, 0.0f, maxScroll);
    if (maxScroll > 0.0f) {
        ui::scrollbar(Rectangle{bounds.x + bounds.width - ui::s(12.0f), view.y, ui::s(7.0f),
                                view.height},
                      &gPaletteScroll, gPaletteContentHeight, view.height);
    }
}

// Captions and block names are drawn after the CrystalGUI buttons so the
// themed boxes cannot wash them out, and clipped so scrolling looks right.
// The scrollable part of the palette panel, shared by the chrome clip and the
// caption/label drawing so they always agree.
Rectangle paletteView(Rectangle bounds) {
    return Rectangle{bounds.x + 1.0f, bounds.y + ui::s(28.0f), bounds.width - 2.0f,
                     bounds.height - ui::s(30.0f)};
}

void drawPaletteLabels(UiState &state, Rectangle bounds) {
    const ui::Theme &t = ui::theme();
    const Rectangle view = paletteView(bounds);
    BeginScissorMode(static_cast<int>(view.x), static_cast<int>(view.y),
                     static_cast<int>(view.width), static_cast<int>(view.height));
    for (const auto &caption : gPaletteCaptions) {
        const float y = bounds.y + ui::s(30.0f) + caption.second - gPaletteScroll;
        ui::drawText(Rectangle{bounds.x + ui::s(12.0f), y, bounds.width - ui::s(24.0f), ui::s(18.0f)},
                     caption.first.c_str(), 11.0f, palette::withAlpha(t.accent, 0.95f),
                     ui::Align::Left, true);
    }
    for (const ChromeRect &entry : gChromeRects) {
        if (entry.topBar) continue;
        ui::drawTextClipped(Rectangle{entry.rect.x + ui::s(12.0f), entry.rect.y,
                                      entry.rect.width - ui::s(20.0f), entry.rect.height},
                            entry.label.c_str(), 12.0f, ui::readableOn(t.controlFill),
                            ui::Align::Left);
    }
    EndScissorMode();
    (void)state;
}

void drawStatusBar(UiState &state, Rectangle bounds) {
    const ui::Theme &t = ui::theme();
    DrawRectangle(static_cast<int>(bounds.x), static_cast<int>(bounds.y),
                  static_cast<int>(bounds.width), static_cast<int>(bounds.height),
                  palette::withAlpha(t.panel, 0.98f));
    DrawLine(static_cast<int>(bounds.x), static_cast<int>(bounds.y),
             static_cast<int>(bounds.x + bounds.width), static_cast<int>(bounds.y), t.border);

    char left[256];
    std::snprintf(left, sizeof(left), "%s%s",
                  state.project.filePath.empty() ? "(unsaved project)" : state.project.filePath.c_str(),
                  state.project.dirty ? " *" : "");
    ui::drawTextClipped(Rectangle{bounds.x + 10.0f, bounds.y, bounds.width * 0.45f, bounds.height},
                        left, 11.0f, t.textDim);

    if (!state.statusMessage.empty() && GetTime() < state.statusUntil) {
        ui::drawTextClipped(Rectangle{bounds.x + bounds.width * 0.45f, bounds.y,
                                      bounds.width * 0.4f, bounds.height},
                            state.statusMessage.c_str(), 11.0f,
                            state.statusIsError ? t.danger : t.success);
    }
    char right[160];
    std::snprintf(right, sizeof(right), "blocks %d   targets %d/%d   shaders %d",
                  state.project.graph.nodeCount(), state.pooledTargets,
                  state.renderer.stats().peakTargets, state.shaderPasses);
    ui::drawTextClipped(Rectangle{bounds.x + bounds.width * 0.85f, bounds.y,
                                  bounds.width * 0.15f - 10.0f, bounds.height},
                        right, 11.0f, t.textDim, ui::Align::Right);
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

namespace {

void computeLayout(UiState &state, float width, float height) {
    const float topHeight = ui::s(38.0f);
    const float statusHeight = ui::s(24.0f);
    const float paletteWidth = ui::s(196.0f);
    // The right column grows with the window so a 2K display gets a usable
    // preview instead of a small one above a mostly empty inspector.
    const float rightWidth = std::clamp(width * 0.26f, ui::s(300.0f), ui::s(560.0f));
    const float timelineHeight = ui::s(196.0f);
    const float gap = ui::s(3.0f);

    state.topBarRect = Rectangle{0, 0, width, topHeight};
    state.statusRect = Rectangle{0, height - statusHeight, width, statusHeight};
    state.paletteRect = Rectangle{gap, topHeight + gap, paletteWidth,
                                  height - topHeight - statusHeight - 2.0f * gap};
    const float centerX = state.paletteRect.x + state.paletteRect.width + gap;
    const float rightX = width - rightWidth - gap;
    const float contentWidth = rightX - centerX - gap;
    const float contentHeight = height - topHeight - statusHeight - 2.0f * gap;
    state.canvasRect =
        Rectangle{centerX, topHeight + gap, contentWidth, contentHeight - timelineHeight - gap};
    state.timelineRect = Rectangle{centerX, state.canvasRect.y + state.canvasRect.height + gap,
                                   contentWidth, timelineHeight};
    const float previewHeight =
        std::clamp(contentHeight * 0.42f, ui::s(260.0f), ui::s(620.0f));
    state.previewRect = Rectangle{rightX, topHeight + gap, rightWidth, previewHeight};
    state.inspectorRect = Rectangle{rightX, state.previewRect.y + previewHeight + gap, rightWidth,
                                    contentHeight - previewHeight - gap};
}

void renderPreviewFrame(UiState &state) {
    const double duration = state.project.effectiveDuration(state.clip.duration());
    EvalContext &ctx = state.frameContext;
    ctx.width = std::max(32, state.project.video.width /
                                 (state.previewScaleIndex == 0 ? 1 : (state.previewScaleIndex == 1 ? 2 : 4)));
    ctx.height = std::max(32, state.project.video.height /
                                  (state.previewScaleIndex == 0 ? 1 : (state.previewScaleIndex == 1 ? 2 : 4)));
    ctx.fps = static_cast<float>(state.project.video.fps);
    ctx.duration = duration;
    ctx.time = state.playhead;
    ctx.frame = static_cast<int>(state.playhead * state.project.video.fps);
    ctx.audioTime = state.project.audioTimeAt(state.playhead, state.clip.duration());
    ctx.audio = state.clip.buffer();
    ctx.analysis = state.analysis;
    ctx.offline = false;

    std::string error;
    state.previewImage = state.renderer.renderFrame(state.project.graph, ctx, &error);
    state.lastRenderMs = state.renderer.stats().frameMs;
    state.shaderPasses = state.renderer.stats().shaderPasses;
    state.pooledTargets = state.renderer.stats().pooledTargets;
    if (!state.previewImage && !error.empty()) {
        static double lastReport = 0.0;
        if (GetTime() - lastReport > 2.0) {
            lastReport = GetTime();
            setStatus(state, error, true);
        }
    }
}

void performExport(UiState &state) {
    if (!state.exporting) return;

    // Dedicated modal loop: the renderer must stay on this thread, so progress
    // is drawn from the frame callback.
    ExportRequest request;
    request.outputPath = state.exportPath;
    request.overwrite = true;

    double lastDraw = 0.0;
    // The export drives the editor's own progress indicators (the timeline
    // playhead and the preview scrub bar) and puts the playhead back where the
    // user left it when the run is over.
    const double playheadBefore = state.playhead;
    bool cancelled = false;
    std::string error;
    const std::vector<unsigned char> *embedded =
        state.clip.hasTranscodedAudio() ? &state.clip.transcodedAudio() : nullptr;
    const bool ok = Exporter::run(
        state.renderer, state.project, request, state.clip.buffer(), state.analysis,
        [&](const ExportProgress &progress) {
            state.exportProgress =
                progress.frameCount > 0
                    ? static_cast<float>(progress.frame) / static_cast<float>(progress.frameCount)
                    : 0.0f;
            char status[192];
            std::snprintf(status, sizeof(status),
                          "frame %d/%d  -  %.1f fps  -  eta %.0f s  -  render %.1f ms",
                          progress.frame, progress.frameCount, progress.fps,
                          std::max(0.0, progress.remaining), progress.renderMs);
            state.exportStatus = status;
            // Updated before the redraw throttle so the bars show the frame the
            // export is actually on, not the frame of the last redraw.
            state.playhead = std::clamp(
                progress.videoTime, 0.0,
                std::max(0.0, state.project.effectiveDuration(state.clip.duration())));
            // The in-block previews (the Audio Source waveform, the LFO and
            // automation pivots, the meters) read the editor's evaluation
            // context, so that has to follow the exported frame too. Without it
            // a Self Reference capture shows a frozen waveform while the
            // playhead moves.
            EvalContext &frame = state.frameContext;
            frame.time = state.playhead;
            frame.frame = progress.frame;
            frame.duration = state.project.effectiveDuration(state.clip.duration());
            frame.audioTime = state.project.audioTimeAt(state.playhead, state.clip.duration());
            frame.audio = state.clip.buffer();
            frame.analysis = state.analysis;

            const double now = GetTime();
            // A Self Reference has to update once per exported frame to stay
            // smooth, so the editor is redrawn for every frame while one is in
            // the graph; otherwise the 12 Hz progress redraw stays in place.
            const bool wantsMirror = graphNeedsScreenCapture(state.project.graph);
            if (!wantsMirror && now - lastDraw < 0.08 &&
                progress.frame != progress.frameCount) {
                return;
            }
            lastDraw = now;

            // The preview pane follows the export frame by frame, so the editor
            // - and a Self Reference capture of it - shows the pipeline live.
            if (state.renderer.outputTarget()) state.previewImage = state.renderer.outputTarget();
            BeginDrawing();
            ui::beginFrame();
            ui::setModal(true);
            // Refreshes the chrome hit test with the modal flag set, so every
            // button paints its disabled state while the export runs.
            updateChromeLayout(state);
            ClearBackground(palette::background());
            // The editor stays visible but frozen, dimmed by the overlay below,
            // so the chrome buttons grey out like everything else.
            drawEditor(state, false);
            // Self Reference must see the pipeline, not the exporter's grey-out:
            // grab the frame before the dim rectangle and the progress panel.
            captureEditorFrame(state, progress.frame);
            DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), palette::withAlpha(BLACK, 0.55f));
            const Rectangle box{(GetScreenWidth() - 560.0f) * 0.5f,
                                (GetScreenHeight() - 150.0f) * 0.5f, 560.0f, 150.0f};
            ui::panel(box, "Exporting");
            ui::progressBar(Rectangle{box.x + 20.0f, box.y + 50.0f, box.width - 40.0f, 22.0f},
                            state.exportProgress, nullptr);
            ui::drawText(Rectangle{box.x + 20.0f, box.y + 80.0f, box.width - 40.0f, 20.0f},
                         state.exportStatus.c_str(), 13.0f, ui::theme().text, ui::Align::Center);
            ui::drawText(Rectangle{box.x + 20.0f, box.y + 104.0f, box.width - 40.0f, 18.0f},
                         "press Escape to cancel", 11.0f, ui::theme().textDim, ui::Align::Center);
            EndDrawing();
        },
        [&]() {
            if (WindowShouldClose() || IsKeyPressed(KEY_ESCAPE)) {
                cancelled = true;
                return true;
            }
            return false;
        },
        &error, embedded);

    state.exporting = false;
    state.exportProgress = 0.0f;
    state.exportStatus.clear();
    state.playhead = playheadBefore;
    if (cancelled) {
        setStatus(state, "Export cancelled", true);
    } else if (!ok) {
        state.exportError = error;
        setStatus(state, "Export failed: " + error, true);
    } else {
        state.exportError.clear();
        setStatus(state, "Exported to " + state.exportPath);
    }
}

}  // namespace

int runApp(int argc, char **argv) {
    // Optional offscreen capture used for automated verification:
    //   PulseForge --shot out.png [--size 2400x1350] [audio.wav]
    std::string shotPath;
    int shotFrames = 10;
    double shotDelay = 0.0;
    int windowWidth = 0;
    int windowHeight = 0;
    bool hiddenWindow = false;
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--shot" && i + 1 < argc) {
            shotPath = argv[++i];
            hiddenWindow = true;
        } else if (argument == "--shot-frames" && i + 1 < argc) {
            shotFrames = std::max(2, std::atoi(argv[++i]));
        } else if (argument == "--shot-delay" && i + 1 < argc) {
            // Wall-clock delay before the capture, e.g. to let a resize or the
            // first few frames settle.
            shotDelay = std::max(0.0, std::atof(argv[++i]));
        } else if (argument == "--hidden") {
            hiddenWindow = true;
        } else if (argument == "--visible") {
            // Keeps the window on screen while capturing, which is what a
            // scripted run needs when it drives the UI with real input.
            hiddenWindow = false;
        } else if (argument == "--size" && i + 1 < argc) {
            // Explicit window size, e.g. --size 2560x1440
            const std::string size = argv[++i];
            const size_t x = size.find('x');
            if (x != std::string::npos) {
                windowWidth = std::atoi(size.substr(0, x).c_str());
                windowHeight = std::atoi(size.substr(x + 1).c_str());
            }
        } else {
            positional.push_back(argument);
        }
    }

    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
    if (hiddenWindow) SetConfigFlags(FLAG_WINDOW_HIDDEN);
    const bool autoSize = windowWidth <= 0 || windowHeight <= 0;
    InitWindow(windowWidth, windowHeight, "PulseForge - programmable audio visualiser");
    if (!IsWindowReady()) {
        TraceLog(LOG_ERROR, "PulseForge: could not create a window");
        return 1;
    }
    // Monitor queries only work once the window exists, so the default size is
    // applied after InitWindow: a 2K display ends up with roughly 2200x1270 of
    // editor, smaller displays get a proportionally smaller window.
    if (autoSize) {
        const int monitor = GetCurrentMonitor();
        const int monitorWidth = GetMonitorWidth(monitor);
        const int monitorHeight = GetMonitorHeight(monitor);
        if (monitorWidth > 0 && monitorHeight > 0) {
            windowWidth = std::clamp(static_cast<int>(monitorWidth * 0.86f), 1280, 2400);
            windowHeight = std::clamp(static_cast<int>(monitorHeight * 0.88f), 800, 1400);
            SetWindowSize(windowWidth, windowHeight);
        } else {
            windowWidth = GetScreenWidth();
            windowHeight = GetScreenHeight();
        }
    }
    SetWindowMinSize(1100, 700);
    SetWindowPosition((GetMonitorWidth(GetCurrentMonitor()) - windowWidth) / 2,
                      (GetMonitorHeight(GetCurrentMonitor()) - windowHeight) / 2);
    SetExitKey(KEY_NULL);

    // CrystalGUI resolves "resource/..." relative to the working directory.
    {
        const char *probe = "resource/shaders/glsl330/box.fs";
        if (!FileExists(probe)) {
            ChangeDirectory(GetApplicationDirectory());
        }
    }
    InitAudioDevice();

    UiState state;
    gApp = &state;
    ui::init(true);
    {
        std::string error;
        if (!state.renderer.init(&error)) {
            TraceLog(LOG_ERROR, "PulseForge: renderer init failed: %s", error.c_str());
            return 1;
        }
    }
    // The Textbox block rasterises the same Inter files the editor atlases use,
    // so video text is sharp at any size instead of upscaling a fixed atlas.
    state.renderer.setDefaultFonts(ui::theme().regularFontPath, ui::theme().boldFontPath);
    state.renderer.shaders().setSearchPaths({"assets/shaders", "assets", "."});
    state.projectDirectory = GetWorkingDirectory();
    state.preferencesPath = state.projectDirectory + "/pulseforge_preferences.json";
    loadPreferences(state.preferences, state.preferencesPath);
    ui::theme().uiScale = state.preferences.guiScale;
    ui::setDarkTheme(state.preferences.defaultDarkTheme);
    state.project.resetToDefault(&state.renderer.shaders());
    selectNode(state, state.project.graph.videoSinkNodeId());
    updateExportExtension(state);  // "output.<container>" next to the project
    // The chrome is positioned in screen coordinates, so the layout has to exist
    // before it is built.
    computeLayout(state, static_cast<float>(GetScreenWidth()), static_cast<float>(GetScreenHeight()));
    buildChrome(state);
    float builtWidth = static_cast<float>(GetScreenWidth());
    float builtHeight = static_cast<float>(GetScreenHeight());

    // Optionally preload an audio file / project from the command line.
    for (size_t i = 0; i < positional.size(); ++i) {
        const std::string &argument = positional[i];
        if (argument.size() > 7 && argument.compare(argument.size() - 7, 7, ".pforge") == 0) {
            loadProjectFile(state, argument);
        } else if (i == 0) {
            loadAudioFile(state, argument);
        }
    }

    double lastFrameTime = GetTime();
    const double startupTime = lastFrameTime;
    int frameCounter = 0;
    while (!WindowShouldClose()) {
        ++frameCounter;
        // Exporting takes over the frame: it renders its own dimmed editor view
        // with the progress panel, so it is handled before anything else.
        if (state.exporting) {
            performExport(state);
            continue;
        }

        // ---- window and layout -------------------------------------------
        // The whole editor follows the window size, so the layout is recomputed
        // every frame; the CrystalGUI chrome is rebuilt whenever the size
        // actually changes because its geometry is baked in screen coordinates.
        {
            const float windowWidthNow = static_cast<float>(GetScreenWidth());
            const float windowHeightNow = static_cast<float>(GetScreenHeight());
            computeLayout(state, windowWidthNow, windowHeightNow);
            if (windowWidthNow != builtWidth || windowHeightNow != builtHeight) {
                builtWidth = windowWidthNow;
                builtHeight = windowHeightNow;
                buildChrome(state);
            }
            // The chrome's ability to handle the mouse depends on the modal
            // state, so that has to be current before the layout is refreshed.
            ui::setModal(modalActive(state));
            updateChromeLayout(state);
        }

        // ---- drag and drop ---------------------------------------------
        if (IsFileDropped()) {
            FilePathList dropped = LoadDroppedFiles();
            for (unsigned int i = 0; i < dropped.count; ++i) {
                handleDroppedFile(state, dropped.paths[i]);
            }
            UnloadDroppedFiles(dropped);
        }

        // ---- transport -------------------------------------------------
        const double duration = state.project.effectiveDuration(state.clip.duration());
        state.clip.updatePreview();
        if (state.playing) {
            // Rewiring the Audio Output while the transport runs takes effect
            // immediately: rebuild the monitor from the new route and resume at
            // the current playhead.
            const std::string route = monitorRouteIdentity(state);
            if (route != state.monitorRoute) {
                state.clip.stopPreview();
                std::string monitorError;
                if (!prepareMonitorAudio(state, &monitorError)) {
                    state.playing = false;
                    setStatus(state, "Playback stopped: " + monitorError, true);
                } else {
                    state.monitorRenderKey = monitorRenderKey(state);
                    state.monitorRoute = route;
                    state.liveAudioFrame = -1;
                    state.clip.seek(state.playhead + state.project.video.trimStart);
                    state.clip.startPreview();
                }
            }
            if (state.playing) {
                state.playhead += GetFrameTime();
                if (state.playhead >= duration) {
                    if (state.loopPlayback) {
                        state.playhead = 0.0;
                        state.liveAudioFrame = -1;
                        state.clip.seek(state.project.video.trimStart);
                        state.clip.startPreview();
                    } else {
                        state.playing = false;
                        state.playhead = duration;
                        state.clip.pausePreview();
                    }
                }
            }
        } else if (state.clip.previewPlaying()) {
            state.playhead = std::max(0.0, state.clip.previewPosition() - state.project.video.trimStart);
        }

        // Render any audio frames the display skipped before the current frame,
        // so the live stream stays continuous on a slow display or a hitch.
        if (state.playing && state.clip.liveStreamActive()) {
            const double fps = std::max(1.0, state.project.video.fps);
            catchUpLiveAudio(
                state, static_cast<int>(std::floor(
                           std::max(0.0, state.playhead) * fps + 1e-6)));
        }

        // ---- render the pipeline ---------------------------------------
        renderPreviewFrame(state);

        // ---- live graph audio ------------------------------------------
        // The region evaluated above just produced this video frame's samples;
        // stream them so the sound follows the video clock frame by frame.
        if (state.playing && state.clip.liveStreamActive()) {
            pushLiveAudioWindow(state);
            state.liveAudioFrame =
                std::max(state.liveAudioFrame, state.frameContext.frame + 1);
        }

        // ---- draw -------------------------------------------------------
        const bool capture = !shotPath.empty() && frameCounter >= shotFrames &&
                             GetTime() >= startupTime + shotDelay;
        RenderTexture2D captureTarget{};
        if (capture) {
            captureTarget = LoadRenderTexture(GetScreenWidth(), GetScreenHeight());
            BeginTextureMode(captureTarget);
        } else {
            BeginDrawing();
        }
        // Widget ids are handed out in draw order, so they must be reset before
        // the panels are drawn.
        ui::beginFrame();
        ClearBackground(palette::background());
        // A dialog freezes the editor behind it; popups are handled inside.
        drawEditor(state, !dialogOpen(state));
        // Self Reference reads this frame; the export overlay is drawn later, in
        // the progress callback, so the grey-out never enters the pipeline.
        captureEditorFrame(state, state.frameContext.frame);

        if (capture) {
            EndTextureMode();
        } else {
            EndDrawing();
        }

        if (capture) {
            // Read back the off-screen frame and flip it into top-down order
            // before writing the PNG.
            FrameReadback readback;
            readback.capture(captureTarget);
            readback.capture(captureTarget);
            const unsigned char *pixels = readback.fetch();
            if (pixels) {
                const int width = captureTarget.texture.width;
                const int height = captureTarget.texture.height;
                std::vector<unsigned char> flipped(static_cast<size_t>(width) * height * 4);
                for (int y = 0; y < height; ++y) {
                    std::memcpy(flipped.data() + static_cast<size_t>(y) * width * 4,
                                pixels + static_cast<size_t>(height - 1 - y) * width * 4,
                                static_cast<size_t>(width) * 4);
                }
                Image image{};
                image.data = flipped.data();
                image.width = width;
                image.height = height;
                image.mipmaps = 1;
                image.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
                if (!ExportImage(image, shotPath.c_str())) {
                    TraceLog(LOG_WARNING, "PulseForge: could not write %s", shotPath.c_str());
                } else {
                    TraceLog(LOG_INFO, "PulseForge: wrote %s", shotPath.c_str());
                }
                readback.release();
            }
            readback.shutdown();
            UnloadRenderTexture(captureTarget);
            break;
        }
        const double now = GetTime();
        const double delta = now - lastFrameTime;
        lastFrameTime = now;
        if (delta > 0.0) state.lastUiFps = state.lastUiFps * 0.9 + (1.0 / delta) * 0.1;

        // ---- deferred work ---------------------------------------------
        if (state.exporting) performExport(state);
    }

    state.clip.clear();
    state.renderer.shutdown();
    if (state.root) {
        CguiDeleteNode(state.root);
        state.root = nullptr;
    }
    ui::shutdown();
    CloseAudioDevice();
    CloseWindow();
    gApp = nullptr;
    return 0;
}

}  // namespace pf
