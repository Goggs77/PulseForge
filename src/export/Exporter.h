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
                                                 bool copyAudio = false);
    static std::string describeCommand(const std::vector<std::string> &arguments);
};

}  // namespace pf
