// Project document: metadata, media references and the pipeline itself.
#pragma once

#include <string>
#include <functional>
#include <vector>

#include "core/Graph.h"
#include "core/Json.h"
#include "render/ShaderLibrary.h"

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
    // Peak magnitude of the decoded clip (derived, not saved). The passthrough
    // ADC -> DAC check uses it to know whether the DAC's clamp would change a
    // single sample; -1 means "not measured".
    float peak = -1.0f;
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

    // Fills in the demo pipeline used by File > New. Loads `template.pforge`
    // next to the executable when it exists (shaders lets its Shader blocks
    // derive their ports); falls back to the built-in pipeline otherwise.
    void resetToDefault(const ShaderLibrary *shaders = nullptr);

    // Adds an Audio Output when the graph has none and wires it to the end of
    // the audio chain the pre-Output exporter used. Returns true when a block
    // was added. Called on load so projects saved before the block existed keep
    // their soundtrack.
    bool ensureAudioOutput();

    // Duration in seconds used for rendering (audio length unless overridden).
    double effectiveDuration(double audioDuration) const;

    // The clip position a playhead maps to: the video trim start plus the
    // playhead, clamped into the clip. The preview loop and the export progress
    // callback share it, so the in-block previews (the Audio Source waveform)
    // read the same time as the frame being shown or rendered.
    double audioTimeAt(double playhead, double clipDuration) const;

    bool save(const std::string &path, std::string *error);
    // `shaders` is optional; it lets a retired "shader.pass" node be migrated to
    // the Shader block with the ports of the .glsl file it points at.
    bool load(const std::string &path, std::string *error,
              const ShaderLibrary *shaders = nullptr);

    json::Value toJson(const std::string &projectDir) const;
    bool fromJson(const json::Value &value, const std::string &projectDir, std::string *error,
                  const ShaderLibrary *shaders = nullptr);

    // Relative-to-project paths when possible, absolute otherwise.
    static std::string toRelative(const std::string &path, const std::string &directory);
    static std::string resolvePath(const std::string &path, const std::string &directory);
    static std::string directoryOf(const std::string &path);
};

// Container presets that fill in codec defaults.
OutputSpec outputSpecForContainer(const std::string &container);
std::vector<std::string> supportedContainers();

// Video encoders offered by the output settings. `id` is what gets stored in the
// project and passed to ffmpeg; the label is what the UI shows.
struct VideoEncoderInfo {
    const char *id;
    const char *label;
    const char *family;  // h264 | hevc | av1 | vp9 | mpeg4
    const char *api;     // cpu | nvenc | qsv | amf
};
const std::vector<VideoEncoderInfo> &videoEncoders();
const VideoEncoderInfo *findVideoEncoder(const std::string &id);
std::string videoEncoderLabel(const std::string &id);
std::string videoEncoderFamily(const std::string &id);
std::string videoEncoderApi(const std::string &id);
bool videoEncoderIsHardware(const std::string &id);
bool containerAcceptsVideoEncoder(const std::string &container, const std::string &id);
std::vector<std::string> videoEncodersForContainer(const std::string &container);

// Audio encoders offered by the output settings. `family` groups the codec
// families the importer matches against (aac, mp3, flac, alac, opus, vorbis,
// ac3, eac3, dts, pcm).
struct AudioEncoderInfo {
    const char *id;
    const char *label;
    const char *family;
};
const std::vector<AudioEncoderInfo> &audioEncoders();
const AudioEncoderInfo *findAudioEncoder(const std::string &id);
std::string audioEncoderLabel(const std::string &id);
std::string audioEncoderFamily(const std::string &id);
// Which codec families a container can mux.
bool containerCarriesAudioFamily(const std::string &container, const std::string &family);
bool containerAcceptsAudioEncoder(const std::string &container, const std::string &id);
std::vector<std::string> audioEncodersForContainer(const std::string &container);

// Codec family of a decoded source stream ("aac", "mp3", "flac", "pcm", ...).
std::string audioSourceFamily(const std::string &sourceCodec);

// The encoder an imported file should be exported with.
struct AudioEncoderChoice {
    std::string encoder;    // ffmpeg encoder name; empty = keep the project's
    int bitrateKbps = 0;    // 0 = keep the container default
    bool transcodeToAac = false;  // media has to be converted to AAC in memory
};
// `isAvailable` reports whether this ffmpeg build carries an encoder (usually
// ffmpeg::hasEncoder), which keeps the decision testable without ffmpeg.
AudioEncoderChoice chooseAudioEncoder(const std::string &sourceCodec, int sourceBitrateKbps,
                                      const std::string &container,
                                      const std::function<bool(const std::string &)> &isAvailable);

// Audio codec helpers shared by the importer, the inspector and the exporter.
bool audioCodecIsLossless(const std::string &codec);
bool containerAcceptsAac(const std::string &container);
std::string audioCodecDisplayName(const std::string &codec);
int parseAudioBitrateKbps(const std::string &bitrate);
// Rewrites an export path so its extension matches the container, defaulting to
// "<directory>/output.<container>" when the path is empty.
std::string exportPathForContainer(const std::string &path, const std::string &container,
                                   const std::string &directory);

}  // namespace pf
