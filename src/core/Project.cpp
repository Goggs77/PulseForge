#include "core/Project.h"

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <unordered_map>

#include "core/Registry.h"
#include "render/Palette.h"

namespace pf {

namespace {

std::string readFile(const std::string &path) {
    std::ifstream file(path.c_str(), std::ios::binary);
    if (!file.good()) return std::string();
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::string paramKindName(ParamKind kind) {
    switch (kind) {
        case ParamKind::Int: return "int";
        case ParamKind::Bool: return "bool";
        case ParamKind::Color: return "color";
        case ParamKind::Text: return "text";
        case ParamKind::Enum: return "enum";
        case ParamKind::File: return "file";
        case ParamKind::Curve: return "curve";
        case ParamKind::Wave: return "wave";
        case ParamKind::Band: return "band";
        default: return "float";
    }
}

json::Value colorToJson(Color color) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X%02X", color.r, color.g, color.b, color.a);
    return json::Value::makeString(buffer);
}

Color colorFromJson(const json::Value &value, Color fallback) {
    const std::string text = value.asString();
    if (text.size() < 7 || text[0] != '#') return fallback;
    unsigned int components[4] = {0, 0, 0, 255};
    const size_t count = std::min<size_t>(4, (text.size() - 1) / 2);
    for (size_t i = 0; i < count; ++i) {
        components[i] = static_cast<unsigned int>(
            std::strtoul(text.substr(1 + i * 2, 2).c_str(), nullptr, 16));
    }
    return Color{static_cast<unsigned char>(components[0]), static_cast<unsigned char>(components[1]),
                 static_cast<unsigned char>(components[2]),
                 static_cast<unsigned char>(count >= 4 ? components[3] : 255)};
}

}  // namespace

std::string Project::directoryOf(const std::string &path) {
    const size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos) return std::string();
    return path.substr(0, slash);
}

std::string Project::toRelative(const std::string &path, const std::string &directory) {
    if (path.empty() || directory.empty()) return path;
    std::string normalized = path;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    std::string base = directory;
    std::replace(base.begin(), base.end(), '\\', '/');
    if (base.back() != '/') base += '/';
    if (normalized.size() > base.size() && normalized.compare(0, base.size(), base) == 0) {
        return normalized.substr(base.size());
    }
    return normalized;
}

std::string Project::resolvePath(const std::string &path, const std::string &directory) {
    if (path.empty()) return path;
    // Absolute Windows paths and POSIX-style rooted paths are used as-is.
    if (path.size() > 1 && path[1] == ':') return path;
    if (!path.empty() && (path[0] == '/' || path[0] == '\\')) return path;
    if (directory.empty()) return path;
    std::string base = directory;
    std::replace(base.begin(), base.end(), '\\', '/');
    if (base.back() != '/') base += '/';
    return base + path;
}

double Project::effectiveDuration(double audioDuration) const {
    double duration = video.duration;
    if (video.useAudioDuration) {
        duration = audioDuration;
        if (video.trimEnd > video.trimStart) duration = video.trimEnd - video.trimStart;
        else duration = std::max(0.0, audioDuration - video.trimStart);
    }
    return std::max(1.0 / 240.0, duration);
}

std::vector<std::string> supportedContainers() { return {"mp4", "webm", "mkv", "mov", "avi"}; }

const std::vector<VideoEncoderInfo> &videoEncoders() {
    // Ordered so the CPU encoders (always available) come first, then the GPU
    // families. The exporter adapts its quality options to `api`.
    static const std::vector<VideoEncoderInfo> encoders = {
        {"libx264", "H.264 (x264)", "h264", "cpu"},
        {"h264_nvenc", "H.264 (NVENC GPU)", "h264", "nvenc"},
        {"h264_qsv", "H.264 (Quick Sync GPU)", "h264", "qsv"},
        {"h264_amf", "H.264 (AMF GPU)", "h264", "amf"},
        {"libx265", "HEVC (x265)", "hevc", "cpu"},
        {"hevc_nvenc", "HEVC (NVENC GPU)", "hevc", "nvenc"},
        {"hevc_qsv", "HEVC (Quick Sync GPU)", "hevc", "qsv"},
        {"hevc_amf", "HEVC (AMF GPU)", "hevc", "amf"},
        {"libsvtav1", "AV1 (SVT)", "av1", "cpu"},
        {"av1_nvenc", "AV1 (NVENC GPU)", "av1", "nvenc"},
        {"av1_qsv", "AV1 (Quick Sync GPU)", "av1", "qsv"},
        {"av1_amf", "AV1 (AMF GPU)", "av1", "amf"},
        {"libvpx-vp9", "VP9 (libvpx)", "vp9", "cpu"},
        {"mpeg4", "MPEG-4", "mpeg4", "cpu"},
    };
    return encoders;
}

const VideoEncoderInfo *findVideoEncoder(const std::string &id) {
    for (const VideoEncoderInfo &encoder : videoEncoders()) {
        if (id == encoder.id) return &encoder;
    }
    return nullptr;
}

std::string videoEncoderLabel(const std::string &id) {
    const VideoEncoderInfo *encoder = findVideoEncoder(id);
    return encoder ? std::string(encoder->label) : id;
}

std::string videoEncoderFamily(const std::string &id) {
    const VideoEncoderInfo *encoder = findVideoEncoder(id);
    return encoder ? std::string(encoder->family) : std::string();
}

std::string videoEncoderApi(const std::string &id) {
    const VideoEncoderInfo *encoder = findVideoEncoder(id);
    return encoder ? std::string(encoder->api) : std::string("cpu");
}

bool videoEncoderIsHardware(const std::string &id) {
    const std::string api = videoEncoderApi(id);
    return api == "nvenc" || api == "qsv" || api == "amf";
}

bool containerAcceptsVideoEncoder(const std::string &container, const std::string &id) {
    const std::string family = videoEncoderFamily(id);
    if (family.empty()) return true;  // unknown codec: let ffmpeg decide
    if (container == "webm") return family == "vp9" || family == "av1";
    if (container == "avi") return family == "h264" || family == "mpeg4";
    if (container == "mp4" || container == "mov") return family != "vp9";
    if (container == "mkv") return true;
    return true;
}

std::vector<std::string> videoEncodersForContainer(const std::string &container) {
    std::vector<std::string> result;
    for (const VideoEncoderInfo &encoder : videoEncoders()) {
        if (containerAcceptsVideoEncoder(container, encoder.id)) result.emplace_back(encoder.id);
    }
    return result;
}

const std::vector<AudioEncoderInfo> &audioEncoders() {
    // FLAC/ALAC before the uncompressed PCM entry: raw PCM is legal in Matroska
    // but many players decode it to silence, so it stays available without being
    // the first thing the list offers.
    static const std::vector<AudioEncoderInfo> encoders = {
        {"aac", "AAC", "aac"},
        {"libopus", "Opus", "opus"},
        {"libvorbis", "Vorbis", "vorbis"},
        {"flac", "FLAC (lossless)", "flac"},
        {"alac", "ALAC (lossless)", "alac"},
        {"libmp3lame", "MP3", "mp3"},
        {"ac3", "AC-3", "ac3"},
        {"eac3", "E-AC-3", "eac3"},
        {"pcm_s16le", "PCM 16-bit (uncompressed)", "pcm"},
    };
    return encoders;
}

const AudioEncoderInfo *findAudioEncoder(const std::string &id) {
    for (const AudioEncoderInfo &encoder : audioEncoders()) {
        if (id == encoder.id) return &encoder;
    }
    return nullptr;
}

std::string audioEncoderLabel(const std::string &id) {
    const AudioEncoderInfo *encoder = findAudioEncoder(id);
    return encoder ? std::string(encoder->label) : audioCodecDisplayName(id);
}

std::string audioEncoderFamily(const std::string &id) {
    const AudioEncoderInfo *encoder = findAudioEncoder(id);
    return encoder ? std::string(encoder->family) : std::string();
}

bool containerCarriesAudioFamily(const std::string &container, const std::string &family) {
    if (family.empty()) return false;
    if (container == "webm") return family == "opus" || family == "vorbis";
    if (container == "avi") {
        return family == "mp3" || family == "ac3" || family == "eac3" || family == "aac" ||
               family == "pcm";
    }
    if (container == "mkv") return true;
    if (container == "mov" || container == "mp4") {
        return family == "aac" || family == "alac" || family == "flac" || family == "mp3" ||
               family == "ac3" || family == "eac3" || (container == "mov" && family == "pcm");
    }
    return family == "aac" || family == "alac" || family == "mp3" || family == "ac3" ||
           family == "eac3";
}

bool containerAcceptsAudioEncoder(const std::string &container, const std::string &id) {
    const std::string family = audioEncoderFamily(id);
    if (family.empty()) return true;  // unknown codec: let ffmpeg decide
    return containerCarriesAudioFamily(container, family);
}

std::vector<std::string> audioEncodersForContainer(const std::string &container) {
    std::vector<std::string> result;
    for (const AudioEncoderInfo &encoder : audioEncoders()) {
        if (containerCarriesAudioFamily(container, encoder.family)) result.emplace_back(encoder.id);
    }
    return result;
}

namespace {

// Source codec families the importer can keep, with the encoder that preserves
// them on export.
struct SourceFamily {
    const char *name;
    const char *encoder;
};
constexpr SourceFamily kSourceFamilies[] = {
    {"aac", "aac"},        {"mp3", "libmp3lame"}, {"flac", "flac"},   {"alac", "alac"},
    {"opus", "libopus"},   {"vorbis", "libvorbis"}, {"ac3", "ac3"},   {"eac3", "eac3"},
    {"dts", "dca"},        {"pcm", "pcm_s16le"},
};

}  // namespace

std::string audioSourceFamily(const std::string &sourceCodec) {
    if (sourceCodec.rfind("pcm_", 0) == 0) return "pcm";
    if (sourceCodec == "mp3" || sourceCodec == "mp3float" || sourceCodec == "mp2") return "mp3";
    if (sourceCodec == "aac" || sourceCodec == "aac_latm") return "aac";
    for (const SourceFamily &family : kSourceFamilies) {
        if (sourceCodec == family.name) return family.name;
    }
    return std::string();
}

AudioEncoderChoice chooseAudioEncoder(const std::string &sourceCodec, int sourceBitrateKbps,
                                      const std::string &container,
                                      const std::function<bool(const std::string &)> &isAvailable) {
    auto available = [&](const std::string &encoder) {
        return !encoder.empty() && (!isAvailable || isAvailable(encoder));
    };
    auto encoderForFamily = [](const std::string &family) {
        for (const SourceFamily &entry : kSourceFamilies) {
            if (family == entry.name) return std::string(entry.encoder);
        }
        return std::string();
    };

    AudioEncoderChoice choice;
    const std::string family = audioSourceFamily(sourceCodec);
    if (!family.empty()) {
        const std::string encoder = encoderForFamily(family);
        if (available(encoder) && containerCarriesAudioFamily(container, family)) {
            choice.encoder = encoder;
            // Raw PCM inside Matroska is legal but many players decode it to
            // silence (and the file is huge): use FLAC there, which is lossless,
            // roughly half the size and widely supported.
            if (encoder == "pcm_s16le" && container == "mkv" && available("flac")) {
                choice.encoder = "flac";
            } else if (!audioCodecIsLossless(encoder) && sourceBitrateKbps > 0) {
                choice.bitrateKbps = sourceBitrateKbps;
            }
            return choice;
        }
    }

    // Fall back to the container's own default, then to AAC.
    const OutputSpec preset = outputSpecForContainer(container);
    const std::string presetFamily = audioEncoderFamily(preset.audioCodec);
    if (available(preset.audioCodec) && containerCarriesAudioFamily(container, presetFamily)) {
        choice.encoder = preset.audioCodec;
        choice.bitrateKbps = parseAudioBitrateKbps(preset.audioBitrate);
    } else if (available("aac") && containerCarriesAudioFamily(container, "aac")) {
        choice.encoder = "aac";
    }

    if (!family.empty() && audioEncoderFamily(choice.encoder) == family) return choice;
    if (available("aac") && containerAcceptsAac(container)) {
        choice.encoder = "aac";
        choice.transcodeToAac = true;
        // "Best quality possible": never below the source, within what AAC can
        // usefully carry.
        choice.bitrateKbps = std::clamp(std::max(sourceBitrateKbps, 256), 256, 512);
    } else if (!choice.encoder.empty() && choice.bitrateKbps <= 0) {
        choice.bitrateKbps = std::max(192, sourceBitrateKbps);
    }
    return choice;
}

OutputSpec outputSpecForContainer(const std::string &container) {
    OutputSpec spec;
    spec.container = container;
    if (container == "webm") {
        spec.videoCodec = "libvpx-vp9";
        spec.audioCodec = "libopus";
        spec.crf = 32;
        spec.preset = "good";
        spec.audioBitrate = "160k";
        spec.pixelFormat = "yuv420p";
    } else if (container == "mov") {
        spec.videoCodec = "libx264";
        spec.audioCodec = "aac";
        spec.crf = 18;
        spec.preset = "medium";
    } else if (container == "mkv") {
        spec.videoCodec = "libx264";
        // AAC is the most widely playable audio codec in Matroska; Opus and FLAC
        // are one click away in the Output section.
        spec.audioCodec = "aac";
        spec.crf = 18;
        spec.audioBitrate = "192k";
    } else if (container == "avi") {
        spec.videoCodec = "mpeg4";
        spec.audioCodec = "libmp3lame";
        spec.crf = 4;  // used as qscale for mpeg4
        spec.audioBitrate = "192k";
    }
    return spec;
}

bool audioCodecIsLossless(const std::string &codec) {
    return codec == "flac" || codec == "alac" || codec == "wavpack" || codec == "ape" ||
           codec.rfind("pcm_", 0) == 0 || codec == "truehd" || codec == "mlp";
}

bool containerAcceptsAac(const std::string &container) {
    // AAC in WebM is not part of the format, so the in-memory AAC fallback is
    // only usable by the other containers.
    return container != "webm";
}

std::string audioCodecDisplayName(const std::string &codec) {
    if (codec == "libmp3lame" || codec == "libshine") return "mp3";
    if (codec == "libopus") return "opus";
    if (codec == "libvorbis") return "vorbis";
    if (codec == "libfdk_aac") return "aac";
    if (codec == "pcm_s16le" || codec == "pcm_s24le" || codec == "pcm_f32le") return "pcm";
    if (codec == "dca") return "dts";
    return codec;
}

int parseAudioBitrateKbps(const std::string &bitrate) {
    if (bitrate.empty()) return 0;
    // Accepts "192k", "192 k", "192000" and "192kb/s".
    const double value = std::atof(bitrate.c_str());
    if (value <= 0.0) return 0;
    const bool perSecond = bitrate.find('k') == std::string::npos &&
                           bitrate.find('K') == std::string::npos && value > 2000.0;
    return static_cast<int>(perSecond ? value / 1000.0 : value);
}

std::string exportPathForContainer(const std::string &path, const std::string &container,
                                   const std::string &directory) {
    if (container.empty()) return path;
    if (path.empty()) {
        return directory.empty() ? "output." + container : directory + "/output." + container;
    }
    // Replace the extension when there is one, otherwise append it. Only the
    // part after the last separator counts, so folder names with dots are safe.
    const size_t slash = path.find_last_of("/\\");
    const size_t dot = path.find_last_of('.');
    const size_t nameStart = slash == std::string::npos ? 0 : slash + 1;
    const bool hasExtension = dot != std::string::npos && dot > nameStart && dot + 1 < path.size();
    if (hasExtension) return path.substr(0, dot + 1) + container;
    return path + "." + container;
}

// ---------------------------------------------------------------------------
// Default project
// ---------------------------------------------------------------------------

void Project::resetToDefault() {
    name = "Untitled";
    filePath.clear();
    video = VideoMetadata{};
    output = outputSpecForContainer("mp4");
    graph.clear();
    view = ViewState{};
    audio = MediaRef{};

    // A compact serpentine layout keeps the whole chain visible without panning.
    // The blocks with live content are taller, so the rows are spaced for that.
    Node *audioNode = graph.addNode("src.audio", 0.0f, 20.0f);
    Node *analyzer = graph.addNode("dsp.analyze", 235.0f, 0.0f);
    Node *bass = graph.addNode("dsp.band", 470.0f, 10.0f);
    Node *spectrum = graph.addNode("render.spectrum", 705.0f, 0.0f);
    Node *geometry = graph.addNode("geom.primitives", 940.0f, 0.0f);
    Node *automation = graph.addNode("mod.automation", 0.0f, 350.0f);
    Node *lfo = graph.addNode("mod.lfo", 235.0f, 340.0f);
    Node *clock = graph.addNode("time.clock", 470.0f, 350.0f);
    Node *post = graph.addNode("fx.postfx", 705.0f, 520.0f);
    Node *output = graph.addNode("out.video", 940.0f, 520.0f);
    if (!audioNode || !analyzer || !bass || !lfo || !clock || !spectrum || !geometry || !post ||
        !output || !automation) {
        return;
    }

    spectrum->setInt("preset", 4);  // radial_spectrum
    spectrum->setFloat("scale", 1.0f);
    spectrum->setColor("colorA", palette::fromHex(0x2E6BFF));
    spectrum->setColor("colorB", palette::fromHex(0xFF3FA4));
    spectrum->setBool("useFeedback", true);
    spectrum->setFloat("feedback", 0.35f);

    geometry->setInt("shape", 2);  // radial bars
    geometry->setInt("count", 96);
    geometry->setFloat("radius", 0.30f);
    geometry->setFloat("thickness", 5.0f);
    geometry->setFloat("spin", 0.03f);
    geometry->setFloat("scaleMod", 0.55f);
    geometry->setFloat("reactivity", 0.8f);
    geometry->setColor("colorA", palette::fromHex(0x66D9FF));
    geometry->setColor("colorB", palette::fromHex(0xFF6EC7));

    lfo->setInt("shape", 0);
    lfo->setFloat("frequency", 0.12f);

    if (Param *curve = automation->find("curve")) {
        curve->keys = {Keyframe{0.0, 0.15f, 3}, Keyframe{0.32, 0.70f, 3}, Keyframe{0.55, 0.25f, 3},
                       Keyframe{0.78, 0.85f, 3}, Keyframe{1.0, 0.35f, 3}};
    }
    automation->setFloat("depth", 0.65f);
    automation->setBool("bipolar", false);

    graph.connect(audioNode->id, 0, analyzer->id, 0);
    graph.connect(analyzer->id, 0, bass->id, 0);     // Analysis -> Frequency Band
    graph.connect(analyzer->id, 0, spectrum->id, 0); // Analysis -> Spectrum
    graph.connect(spectrum->id, 0, geometry->id, 0);
    graph.connect(analyzer->id, 3, geometry->id, 1);
    graph.connect(lfo->id, 0, geometry->id, 2);
    graph.connect(geometry->id, 0, post->id, 0);
    graph.connect(automation->id, 0, post->id, 1);  // automation -> bloom
    graph.connect(post->id, 0, output->id, 0);
    dirty = false;
}

// ---------------------------------------------------------------------------
// Serialisation
// ---------------------------------------------------------------------------

json::Value Project::toJson(const std::string &projectDir) const {
    json::Value root = json::Value::makeObject();
    root.set("application", "PulseForge");
    root.set("version", 1);
    root.set("name", name);

    json::Value meta = json::Value::makeObject();
    meta.set("width", video.width);
    meta.set("height", video.height);
    meta.set("fps", video.fps);
    meta.set("useAudioDuration", video.useAudioDuration);
    meta.set("duration", video.duration);
    meta.set("trimStart", video.trimStart);
    meta.set("trimEnd", video.trimEnd);
    root.set("video", meta);

    json::Value outSpec = json::Value::makeObject();
    outSpec.set("container", output.container);
    outSpec.set("videoCodec", output.videoCodec);
    outSpec.set("audioCodec", output.audioCodec);
    outSpec.set("crf", output.crf);
    outSpec.set("preset", output.preset);
    outSpec.set("audioBitrate", output.audioBitrate);
    outSpec.set("audioSampleRate", output.audioSampleRate);
    outSpec.set("bitrateKbps", output.bitrateKbps);
    outSpec.set("pixelFormat", output.pixelFormat);
    outSpec.set("fastStart", output.fastStart);
    root.set("output", outSpec);

    json::Value media = json::Value::makeObject();
    media.set("path", toRelative(audio.path, projectDir));
    media.set("duration", audio.duration);
    media.set("sampleRate", audio.sampleRate);
    media.set("channels", audio.channels);
    media.set("codec", audio.codec);
    media.set("bitRate", static_cast<double>(audio.bitRate));
    media.set("transcodedAac", audio.transcodedAac);
    media.set("transcodedRate", audio.transcodedRate);
    media.set("fileSize", static_cast<double>(audio.fileSize));
    root.set("media", media);

    json::Value viewValue = json::Value::makeObject();
    viewValue.set("panX", view.panX);
    viewValue.set("panY", view.panY);
    viewValue.set("zoom", view.zoom);
    viewValue.set("selectedNode", view.selectedNode);
    viewValue.set("playhead", view.playhead);
    root.set("view", viewValue);

    json::Value nodes = json::Value::makeArray();
    for (const auto &node : graph.nodes) {
        json::Value item = json::Value::makeObject();
        item.set("id", node.id);
        item.set("kind", node.kind);
        item.set("title", node.displayTitle());
        item.set("x", node.x);
        item.set("y", node.y);
        item.set("enabled", node.enabled);
        json::Value params = json::Value::makeObject();
        for (const auto &param : node.params) {
            switch (param.kind) {
                case ParamKind::Bool:
                    params.set(param.key, param.boolean);
                    break;
                case ParamKind::Color:
                    params.set(param.key, colorToJson(param.color));
                    break;
                case ParamKind::Text:
                case ParamKind::File:
                    params.set(param.key, param.text);
                    break;
                case ParamKind::Enum:
                    params.set(param.key, param.intValue());
                    break;
                case ParamKind::Curve: {
                    json::Value keys = json::Value::makeArray();
                    for (const auto &key : param.keys) {
                        json::Value keyValue = json::Value::makeObject();
                        keyValue.set("t", key.time);
                        keyValue.set("v", key.value);
                        keyValue.set("e", key.easing);
                        keys.push(std::move(keyValue));
                    }
                    params.set(param.key, std::move(keys));
                    break;
                }
                default:
                    params.set(param.key, param.value);
                    break;
            }
        }
        item.set("params", std::move(params));
        nodes.push(std::move(item));
    }
    root.set("blocks", std::move(nodes));

    json::Value links = json::Value::makeArray();
    for (const auto &link : graph.links) {
        json::Value item = json::Value::makeObject();
        item.set("from", link.fromNode);
        item.set("fromPort", link.fromPort);
        item.set("to", link.toNode);
        item.set("toPort", link.toPort);
        links.push(std::move(item));
    }
    root.set("connections", std::move(links));
    return root;
}

bool Project::fromJson(const json::Value &root, const std::string &projectDir, std::string *error,
                       const ShaderLibrary *shaders) {
    if (!root.isObject()) {
        if (error) *error = "project file is not a JSON object";
        return false;
    }
    name = root["name"].asString("Untitled");

    const json::Value &meta = root["video"];
    video.width = std::max(16, meta["width"].asInt(1920));
    video.height = std::max(16, meta["height"].asInt(1080));
    video.fps = std::clamp(meta["fps"].asDouble(60.0), 1.0, 480.0);
    video.useAudioDuration = meta["useAudioDuration"].asBool(true);
    video.duration = meta["duration"].asDouble(30.0);
    video.trimStart = meta["trimStart"].asDouble(0.0);
    video.trimEnd = meta["trimEnd"].asDouble(0.0);

    const json::Value &outSpec = root["output"];
    output = outputSpecForContainer(outSpec["container"].asString("mp4"));
    output.videoCodec = outSpec["videoCodec"].asString(output.videoCodec);
    output.audioCodec = outSpec["audioCodec"].asString(output.audioCodec);
    output.crf = outSpec["crf"].asInt(output.crf);
    output.preset = outSpec["preset"].asString(output.preset);
    output.audioBitrate = outSpec["audioBitrate"].asString(output.audioBitrate);
    output.audioSampleRate = outSpec["audioSampleRate"].asInt(output.audioSampleRate);
    output.bitrateKbps = outSpec["bitrateKbps"].asInt(0);
    output.pixelFormat = outSpec["pixelFormat"].asString(output.pixelFormat);
    output.fastStart = outSpec["fastStart"].asBool(true);

    const json::Value &media = root["media"];
    audio = MediaRef{};
    audio.path = resolvePath(media["path"].asString(), projectDir);
    audio.duration = media["duration"].asDouble(0.0);
    audio.sampleRate = media["sampleRate"].asInt(0);
    audio.channels = media["channels"].asInt(0);
    audio.codec = media["codec"].asString();
    audio.bitRate = static_cast<long long>(media["bitRate"].asDouble(0.0));
    audio.transcodedAac = media["transcodedAac"].asBool(false);
    audio.transcodedRate = media["transcodedRate"].asInt(0);
    audio.fileSize = static_cast<long long>(media["fileSize"].asDouble(-1.0));

    const json::Value &viewValue = root["view"];
    view.panX = viewValue["panX"].asFloat(40.0f);
    view.panY = viewValue["panY"].asFloat(40.0f);
    view.zoom = std::clamp(viewValue["zoom"].asFloat(1.0f), 0.15f, 4.0f);
    view.selectedNode = viewValue["selectedNode"].asInt(-1);
    view.playhead = viewValue["playhead"].asDouble(0.0);

    graph.clear();
    const json::Value &blocks = root["blocks"];
    // The retired "shader.pass" block was split into Spectrum (built-in effect,
    // Analysis in) and Shader (.glsl file, Image in). Nodes are migrated by
    // whether they carried a file, and their links are remapped by port name.
    struct LegacyNode {
        std::vector<int> portMap;   // old port index -> new port index, -1 = gone
        bool rewireAnalysis = false;
    };
    std::unordered_map<int, LegacyNode> legacy;
    std::vector<int> migratedSpectrumNodes;
    for (size_t i = 0; i < blocks.size(); ++i) {
        const json::Value &item = blocks.at(i);
        const std::string kind = item["kind"].asString();
        const int id = item["id"].asInt(0);
        // Blocks renamed between versions are migrated here so old projects
        // still load. "dsp.math" became the Math category.
        std::string resolvedKind = kind == "dsp.math" ? "math.arithmetic" : kind;
        LegacyNode migration;
        const bool legacyShaderPass = kind == "shader.pass";
        if (legacyShaderPass) {
            const std::string file = item["params"]["shader"].asString();
            resolvedKind = file.empty() ? "render.spectrum" : "render.shader";
        }
        Node *node = graph.addNodeWithId(id > 0 ? id : graph.nextId, resolvedKind);
        if (!node) continue;
        node->x = item["x"].asFloat(0.0f);
        node->y = item["y"].asFloat(0.0f);
        node->enabled = item["enabled"].asBool(true);
        // The retired block was called "Shader Pass"; keep real custom names but
        // let the default title follow the new label ("Spectrum").
        const std::string storedTitle = item["title"].asString(node->def->label);
        const std::string title =
            legacyShaderPass && storedTitle == "Shader Pass" ? node->def->label : storedTitle;
        if (!title.empty() && title != node->def->label) node->title = title;

        const json::Value &params = item["params"];
        for (Param &param : node->params) {
            if (!params.has(param.key)) continue;
            const json::Value &value = params[param.key];
            switch (param.kind) {
                case ParamKind::Bool: param.boolean = value.asBool(param.boolean); break;
                case ParamKind::Color: param.color = colorFromJson(value, param.color); break;
                case ParamKind::Enum:
                    if (value.isNumber()) param.value = static_cast<float>(value.asInt());
                    else param.value = static_cast<float>(std::atoi(value.asString("0").c_str()));
                    break;
                case ParamKind::Text:
                case ParamKind::File: param.text = value.asString(param.text); break;
                case ParamKind::Curve: {
                    if (!value.isArray() || value.size() == 0) break;
                    param.keys.clear();
                    for (size_t k = 0; k < value.size(); ++k) {
                        const json::Value &entry = value.at(k);
                        Keyframe key;
                        key.time = entry["t"].asDouble(0.0);
                        key.value = entry["v"].asFloat(0.0f);
                        key.easing = entry["e"].asInt(0);
                        param.keys.push_back(key);
                    }
                    break;
                }
                default: param.value = value.asFloat(param.value); break;
            }
        }
        node->ensureParams(*node->def);
        // The Shader block's ports follow its file, so they have to exist before
        // the saved connections are validated below.
        if (resolvedKind == "render.shader" && shaders) {
            Registry::applyShaderPorts(*node, *shaders);
        }
        if (legacyShaderPass) {
            if (resolvedKind == "render.shader") {
                // Old ports: Prev, u0..u7, Vector2, Vector3, Vector4, Matrix.
                const char *names[] = {"Prev",  "u0",       "u1",       "u2",
                                       "u3",    "u4",       "u5",       "u6",
                                       "u7",    "Vector2",  "Vector3",  "Vector4",
                                       "Matrix"};
                migration.portMap.assign(13, -1);
                const std::vector<PortDesc> &ports = node->inputPorts();
                for (int old = 0; old < 13; ++old) {
                    std::string wanted = names[old];
                    if (old >= 1 && old <= 8) {
                        wanted = "uUser[" + std::to_string(old - 1) + "]";
                    }
                    for (size_t port = 0; port < ports.size(); ++port) {
                        if (ports[port].name == wanted) {
                            migration.portMap[static_cast<size_t>(old)] =
                                static_cast<int>(port);
                            break;
                        }
                    }
                }
            } else {
                // Spectrum takes an Analysis input; an old analysis link is
                // rewired onto it, everything else (scalars into uUser) is gone.
                migration.rewireAnalysis = true;
                migratedSpectrumNodes.push_back(node->id);
            }
            legacy[id > 0 ? id : node->id] = migration;
        }
    }

    const json::Value &connections = root["connections"];
    for (size_t i = 0; i < connections.size(); ++i) {
        const json::Value &item = connections.at(i);
        const int fromNode = item["from"].asInt();
        const int fromPort = item["fromPort"].asInt();
        const int toNode = item["to"].asInt();
        int toPort = item["toPort"].asInt();
        const auto legacyIt = legacy.find(toNode);
        if (legacyIt != legacy.end()) {
            const LegacyNode &mapping = legacyIt->second;
            int mapped = (toPort >= 0 && toPort < static_cast<int>(mapping.portMap.size()))
                             ? mapping.portMap[static_cast<size_t>(toPort)]
                             : -1;
            if (mapped < 0 && mapping.rewireAnalysis) {
                const Node *source = graph.find(fromNode);
                const bool analysisSource =
                    source && fromPort >= 0 &&
                    fromPort < static_cast<int>(source->outputPorts().size()) &&
                    source->outputPorts()[static_cast<size_t>(fromPort)].type ==
                        PortType::Analysis;
                if (analysisSource) mapped = 0;
            }
            if (mapped < 0) continue;  // that input does not exist any more
            toPort = mapped;
        }
        graph.connect(fromNode, fromPort, toNode, toPort);
    }

    // A migrated Spectrum block used to read the global analysis; point it at
    // the project's analyzer so it keeps rendering after the split.
    for (const int id : migratedSpectrumNodes) {
        if (!graph.find(id) || graph.findInputLink(id, 0)) continue;
        for (const Node &candidate : graph.nodes) {
            if (candidate.kind != "dsp.analyze") continue;
            const std::vector<PortDesc> &outputs = candidate.outputPorts();
            for (size_t port = 0; port < outputs.size(); ++port) {
                if (outputs[port].type != PortType::Analysis) continue;
                graph.connect(candidate.id, static_cast<int>(port), id, 0);
                break;
            }
            break;
        }
    }
    dirty = false;
    return true;
}

bool Project::save(const std::string &path, std::string *error) {
    const std::string directory = directoryOf(path);
    const json::Value root = toJson(directory);
    std::ofstream file(path.c_str(), std::ios::binary);
    if (!file.good()) {
        if (error) *error = "cannot write " + path;
        return false;
    }
    file << json::write(root, 2);
    file.close();
    filePath = path;
    dirty = false;
    return true;
}

bool Project::load(const std::string &path, std::string *error, const ShaderLibrary *shaders) {
    const std::string text = readFile(path);
    if (text.empty()) {
        if (error) *error = "cannot read " + path;
        return false;
    }
    json::Value root;
    std::string parseError;
    if (!json::parse(text, root, &parseError)) {
        if (error) *error = "invalid project file: " + parseError;
        return false;
    }
    if (!fromJson(root, directoryOf(path), error, shaders)) return false;
    filePath = path;
    dirty = false;
    return true;
}

}  // namespace pf
