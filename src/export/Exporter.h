// Renders the pipeline frame by frame and pipes raw frames into ffmpeg.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "core/Project.h"
#include "render/Renderer.h"

namespace pf {

struct ExportRequest {
    std::string outputPath;
    bool overwrite = true;
    double startTime = 0.0;
    double endTime = 0.0;  // 0 = until the end of the project
    int audioSampleRate = 48000;
};

struct ExportProgress {
    int frame = 0;
    int frameCount = 0;
    double videoTime = 0.0;
    double elapsed = 0.0;
    double remaining = 0.0;
    double renderMs = 0.0;
    double encodeMs = 0.0;
    double fps = 0.0;
    std::string status;
};

// How the export and the monitor get their soundtrack. Source = play or mux the
// media file, Processed = render the graph's Audio Output, Silent = no Audio
// Output or nothing wired into it.
enum class AudioRoute { Source, Processed, Silent };

class Exporter {
public:
    // Blocking; must be called with an active GL context on the main thread.
    // onProgress and shouldCancel are invoked between frames.
    static bool run(Renderer &renderer, Project &project, const ExportRequest &request,
                    const AudioPtr &audio, const AnalysisPtr &analysis,
                    const std::function<void(const ExportProgress &)> &onProgress,
                    const std::function<bool()> &shouldCancel, std::string *error,
                    const std::vector<unsigned char> *embeddedAudio = nullptr);

    // Builds the ffmpeg argument list for the current project/output settings.
    static std::vector<std::string> buildCommand(const Project &project,
                                                 const ExportRequest &request, int frameCount,
                                                 const std::string &audioPathOverride = {},
                                                 bool copyAudio = false, double audioSeek = -1.0,
                                                 bool muteAudio = false);
    static std::string describeCommand(const std::vector<std::string> &arguments);

    // Classifies the Audio Output wiring without rendering anything.
    static AudioRoute audioRoute(const Project &project);

    // Drops every node's rendered audio buffer. A buffer is only valid for the
    // pass that filled it: an offline pass leaves whole-track buffers whose
    // windows the preview's reuse check would accept as fresh, which silenced
    // playback after an export.
    static void resetRenderedAudio(Graph &graph);

    // Renders the graph's Audio Output over [startTime, endTime) for the
    // processed route (the monitor uses it to play what the export will mux).
    // `rendered` is left empty for the source and silent routes. Returns false
    // and fills `error` when the route cannot produce audio.
    static bool renderOutputAudio(Project &project, const AudioPtr &audio,
                                  const AnalysisPtr &analysis, double startTime, double endTime,
                                  const std::function<void(const ExportProgress &)> &onProgress,
                                  const std::function<bool()> &shouldCancel, AudioPtr *rendered,
                                  std::string *error);
};

}  // namespace pf
