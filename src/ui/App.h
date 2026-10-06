// Editor state shared by the panels, plus the application entry point.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/Project.h"
#include "dsp/AudioClip.h"
#include "raylib.h"
#include "render/Renderer.h"
#include "ui/Widgets.h"

struct CguiNode;

namespace pf {

struct BrowserState {
    bool open = false;
    std::string title;
    std::string purpose;      // "audio" | "shader" | "project-open" | "project-save" | "export"
    std::string directory;
    std::string filter;       // ".glsl", ".wav,.mp3", ...
    std::string fileName;
    // Parameter the "node-file" purpose writes the chosen path into.
    std::string paramKey;
    std::vector<std::string> entryNames;
    std::vector<bool> entryIsDirectory;
    float scroll = 0.0f;
    std::string error;
};

struct CanvasState {
    int hoveredNode = -1;
    int hoveredPortNode = -1;
    int hoveredPortIndex = -1;
    bool hoveredPortIsInput = false;

    bool draggingNode = false;
    int dragNodeId = -1;
    Vector2 dragOffset{};

    bool panning = false;

    bool draggingLink = false;
    int linkFromNode = -1;
    int linkFromPort = -1;

    // A block's live content can own the mouse (the Signal Filter's pivot).
    int hoveredVisualNode = -1;
    int visualDragNode = -1;

    std::string lastConnectError;
};

// Application preferences, persisted next to the executable.
struct Preferences {
    bool defaultDarkTheme = true;
    float guiScale = 1.25f;
    // Decode and analyse imported audio on a worker thread, so opening a project
    // or dropping a file does not block the editor; the timeline and the preview
    // stay greyed out until the clip is ready.
    bool asyncAudioLoad = true;
};

void loadPreferences(Preferences &preferences, const std::string &path);
bool savePreferences(const Preferences &preferences, const std::string &path);

struct UiState {
    Project project;
    Renderer renderer;
    AudioClip clip;
    AnalysisPtr analysis;
    ImageBufferPtr previewImage;
    EvalContext frameContext;

    // transport
    bool playing = false;
    double playhead = 0.0;
    bool loopPlayback = true;
    // Monitor playback follows the Audio Output. `monitorRenderKey` caches the
    // pre-rendered track (full graph fingerprint), `monitorRoute` is the cheap
    // wiring signature used to notice a rewire while playing.
    std::string monitorRenderKey;
    std::string monitorRoute;
    // Next video frame index the live audio region has to render. When the
    // display skips a project frame, the missing windows are rendered before
    // the current one so the stream never develops a gap.
    int liveAudioFrame = -1;

    int selectedNode = -1;
    float inspectorScroll = 0.0f;

    // Slice automation handle currently dragged on the timeline: the node id
    // and which end (0 = Start, 1 = Finish). 0 = nothing being dragged.
    int sliceDragNode = 0;
    int sliceDragEnd = 0;
    // Mouse offset from the handle at the moment of the grab, so clicking a
    // handle does not nudge it.
    float sliceDragGrab = 0.0f;
    // Vertical scroll of the timeline's slice lanes when they do not all fit.
    float timelineScroll = 0.0f;
    // Video frame the Self Reference capture was last taken for, so the mirror
    // updates once per video frame instead of once per drawn display frame.
    int lastCaptureFrame = -1;
    // Off-screen target for the export hyper resolution: the editor is drawn
    // into it at the Self Reference's multiple of the window size, then blitted
    // into the renderer's capture. Kept between frames and rebuilt when the size
    // changes; unused (and so never created) while the factor is 1.
    RenderTexture2D editorTarget{};

    // Background audio loading ("Async audio loading" preference). The graph is
    // live immediately; the worker only produces the decoded clip, its analysis
    // and the derived values, and the main thread adopts them once `finished` is
    // set (see pollAudioLoad in App.cpp). `audioLoading` greys the timeline and
    // the preview out while it runs.
    struct AudioLoadTask {
        std::thread worker;
        std::atomic<bool> finished{false};
        AudioPtr buffer;
        AnalysisPtr analysis;
        std::vector<float> overviewMin;
        std::vector<float> overviewMax;
        float peak = 0.0f;
        std::string path;
        std::string error;
        // Runs on the main thread once the clip has been adopted.
        std::function<void(UiState &)> onDone;

        // Clears the task for reuse; the caller joins `worker` first (an atomic
        // is not movable, so the task cannot be reassigned wholesale).
        void reset() {
            worker = std::thread{};
            finished.store(false, std::memory_order_relaxed);
            buffer.reset();
            analysis.reset();
            overviewMin.clear();
            overviewMax.clear();
            peak = 0.0f;
            path.clear();
            error.clear();
            onDone = nullptr;
        }
    };
    AudioLoadTask audioLoad;
    bool audioLoading = false;

    // layout
    Rectangle topBarRect{};
    Rectangle paletteRect{};
    Rectangle canvasRect{};
    Rectangle timelineRect{};
    Rectangle previewRect{};
    Rectangle inspectorRect{};
    Rectangle statusRect{};
    bool layoutDirty = true;

    CanvasState canvas;
    BrowserState browser;

    // dialogs
    bool showExportDialog = false;
    bool showPreferences = false;
    bool showMessage = false;
    std::string messageTitle;
    std::string messageText;
    bool messageIsError = false;
    // When set, the message box asks for confirmation and runs this on Continue.
    std::function<void()> messageConfirm;
    Preferences preferences;
    std::string preferencesPath;
    std::string windowTitle;
    bool showHelp = false;
    std::string exportPath;
    std::string exportLog;
    std::string exportError;
    bool exporting = false;
    float exportProgress = 0.0f;
    std::string exportStatus;

    // notifications
    std::string statusMessage;
    double statusUntil = 0.0;
    bool statusIsError = false;

    // stats
    double lastRenderMs = 0.0;
    double lastUiFps = 0.0;
    int shaderPasses = 0;
    int pooledTargets = 0;
    // Last shader path a Shader block's ports were derived from, keyed by block.
    std::unordered_map<int, std::string> shaderPortKey;

    // analysis progress
    bool analyzing = false;
    float analysisProgress = 0.0f;

    std::string projectDirectory;
    std::string resourceDirectory;
    int previewScaleIndex = 0;   // 0 = full, 1 = half, 2 = quarter

    // CrystalGUI chrome
    CguiNode *root = nullptr;
};

void setStatus(UiState &state, const std::string &message, bool error = false);

// panels
void drawTopBar(UiState &state, Rectangle bounds);
void drawPalette(UiState &state, Rectangle bounds);
void drawGraphCanvas(UiState &state, Rectangle bounds);
void drawTimeline(UiState &state, Rectangle bounds);
void drawInspector(UiState &state, Rectangle bounds);
void drawPreview(UiState &state, Rectangle bounds);
void drawStatusBar(UiState &state, Rectangle bounds);
void drawExportDialog(UiState &state);
void drawPreferencesDialog(UiState &state);
void drawMessageBox(UiState &state);
void drawHelpOverlay(UiState &state);
void drawBrowser(UiState &state);
void refreshBrowserListing(BrowserState &browser);

// actions shared by the panels
void newProject(UiState &state);
void loadAudioFile(UiState &state, const std::string &path);
void loadProjectFile(UiState &state, const std::string &path);
void saveProjectFile(UiState &state, const std::string &path);
void startExport(UiState &state, const std::string &path);
// Prepares the monitor for the current Audio Output route and starts playback.
// The routed track is rendered once and cached until the graph changes.
void startPlayback(UiState &state);
void pausePlayback(UiState &state);
void stopPlayback(UiState &state);
void openBrowser(UiState &state, const std::string &purpose, const std::string &title,
                 const std::string &filter, const std::string &initialPath);
void deleteSelectedNode(UiState &state);
// Changes the selected block, resetting the inspector scroll and dropping any
// in-progress edit so its state cannot leak into another block's widgets.
void selectNode(UiState &state, int nodeId);
void addNodeFromKind(UiState &state, const std::string &kind);
void showMessage(UiState &state, const std::string &title, const std::string &text, bool error);
// Reminder with Continue/Cancel; `onConfirm` runs when the user continues.
void showConfirm(UiState &state, const std::string &title, const std::string &text, bool error,
                 std::function<void()> onConfirm);
// Re-derives the output audio codec/bitrate from the imported media, e.g. after
// the container changes. Never transcodes; it only re-points the settings.
void refreshOutputAudio(UiState &state);
// Rebuilds a Shader block's input ports from its .glsl file. Cached per block,
// so calling it every frame only rescans when the path changed (or `force`).
void refreshShaderPorts(UiState &state, Node &node, bool force = false);
// Starts the render for the current export path, first asking for confirmation
// when the audio settings would audibly downgrade the imported media.
void requestExport(UiState &state);
// Keeps the export file name's extension in step with the output container, so
// switching to WebM does not keep writing "output.mp4".
void updateExportExtension(UiState &state);
Rectangle nodeBounds(const Graph &graph, const Node &node);

int runApp(int argc, char **argv);

}  // namespace pf
