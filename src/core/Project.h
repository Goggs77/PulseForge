// Project document: metadata, media references and the pipeline itself.
#pragma once

#include <string>
#include <vector>

#include "core/Graph.h"
#include "core/Json.h"

namespace pf {

struct VideoMetadata {
    int width = 1920;
    int height = 1080;
    double fps = 60.0;
    bool useAudioDuration = true;
    double duration = 30.0;
    double trimStart = 0.0;
    double trimEnd = 0.0;  // 0 = use the whole clip
};

struct OutputSpec {
    std::string container = "mp4";
    std::string videoCodec = "libx264";
    std::string audioCodec = "aac";
    int crf = 18;
    std::string preset = "medium";
    std::string audioBitrate = "192k";
    int audioSampleRate = 48000;  // output rate, follows the media on import when unset
    int bitrateKbps = 0;  // 0 = constant quality
    std::string pixelFormat = "yuv420p";
    bool fastStart = true;
};

struct MediaRef {
    std::string path;      // as stored in the project (may be relative)
    std::string absolute;  // resolved for the current session
    double duration = 0.0;
    int sampleRate = 0;
    int channels = 0;
    std::string codec;
    long long bitRate = 0;      // source bitrate in bits/s, 0 when unknown/lossless
    bool transcodedAac = false; // media was converted to AAC in memory on import
    int transcodedRate = 0;     // sample rate of that in-memory AAC stream
    long long fileSize = -1;
};

struct ViewState {
    float panX = 40.0f;
    float panY = 40.0f;
    float zoom = 1.0f;
    int selectedNode = -1;
    double playhead = 0.0;
    bool followPlayhead = true;
};

class Project {
public:
    std::string name = "Untitled";
    std::string filePath;
    VideoMetadata video;
    OutputSpec output;
    MediaRef audio;
    Graph graph;
    ViewState view;
    bool dirty = false;

    // Fills in the demo pipeline used by File > New.
    void resetToDefault();

    // Duration in seconds used for rendering (audio length unless overridden).
    double effectiveDuration(double audioDuration) const;

    bool save(const std::string &path, std::string *error);
    bool load(const std::string &path, std::string *error);

    json::Value toJson(const std::string &projectDir) const;
    bool fromJson(const json::Value &value, const std::string &projectDir, std::string *error);

    // Relative-to-project paths when possible, absolute otherwise.
    static std::string toRelative(const std::string &path, const std::string &directory);
    static std::string resolvePath(const std::string &path, const std::string &directory);
    static std::string directoryOf(const std::string &path);
};

// Container presets that fill in codec defaults.
OutputSpec outputSpecForContainer(const std::string &container);
std::vector<std::string> supportedContainers();

// Audio codec helpers shared by the importer, the inspector and the exporter.
bool audioCodecIsLossless(const std::string &codec);
bool containerAcceptsAac(const std::string &container);
std::string audioCodecDisplayName(const std::string &codec);
int parseAudioBitrateKbps(const std::string &bitrate);

}  // namespace pf
