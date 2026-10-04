#include "export/Exporter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <utility>

#include "export/FFmpeg.h"
#include "raylib.h"
#include "render/FrameReadback.h"

namespace pf {

namespace {

// Returns the node feeding the project's Audio Output: 0 when the graph has no
// Audio Output block, -1 when the block exists but nothing is wired into it.
// Export follows this link exclusively; the pre-Output exporter instead reached
// into the graph for the last Dynamics node.
int audioOutputSource(const Graph &graph) {
    const int sinkId = graph.audioSinkNodeId();
    if (sinkId == 0) return 0;
    const Node *sink = graph.find(sinkId);
    if (!sink || !sink->enabled) return -1;
    const Link *link = graph.findInputLink(sinkId, 0);
    if (!link) return -1;
    const Node *source = graph.find(link->fromNode);
    if (!source || !source->enabled) return -1;
    return source->id;
}

void writeLE16(std::ofstream &out, unsigned short value) {
    const unsigned char bytes[2] = {static_cast<unsigned char>(value & 0xFF),
                                    static_cast<unsigned char>((value >> 8) & 0xFF)};
    out.write(reinterpret_cast<const char *>(bytes), 2);
}

void writeLE32(std::ofstream &out, unsigned int value) {
    const unsigned char bytes[4] = {static_cast<unsigned char>(value & 0xFF),
                                    static_cast<unsigned char>((value >> 8) & 0xFF),
                                    static_cast<unsigned char>((value >> 16) & 0xFF),
                                    static_cast<unsigned char>((value >> 24) & 0xFF)};
    out.write(reinterpret_cast<const char *>(bytes), 4);
}

// 32-bit float WAV for the graph-processed audio. ffmpeg reads it directly and
// keeps the full precision of the DSP chain.
bool writeFloatWav(const std::filesystem::path &path, const AudioBuffer &buffer,
                   long long startFrame, long long frames, std::string *error) {
    const int channels = std::max(1, buffer.channels);
    const long long available = std::max<long long>(0, buffer.frameCount - startFrame);
    frames = std::clamp<long long>(frames, 0, available);
    const unsigned long long dataBytes =
        static_cast<unsigned long long>(frames) * channels * sizeof(float);
    if (frames <= 0 || dataBytes + 36ull > 0xFFFFFFFFull) {
        if (error) *error = frames <= 0 ? "the graph produced no audio" : "processed audio is too large";
        return false;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.good()) {
        if (error) *error = "could not write " + path.string();
        return false;
    }
    out.write("RIFF", 4);
    writeLE32(out, static_cast<unsigned int>(36 + dataBytes));
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    writeLE32(out, 16);
    writeLE16(out, 3);  // IEEE float
    writeLE16(out, static_cast<unsigned short>(channels));
    writeLE32(out, static_cast<unsigned int>(std::max(8000, buffer.sampleRate)));
    writeLE32(out, static_cast<unsigned int>(std::max(8000, buffer.sampleRate) * channels * 4));
    writeLE16(out, static_cast<unsigned short>(channels * 4));
    writeLE16(out, 32);
    out.write("data", 4);
    writeLE32(out, static_cast<unsigned int>(dataBytes));
    const size_t offset = static_cast<size_t>(startFrame) * channels;
    out.write(reinterpret_cast<const char *>(buffer.samples.data() + offset),
              static_cast<std::streamsize>(dataBytes));
    if (!out.good()) {
        if (error) *error = "failed while writing " + path.string();
        return false;
    }
    return true;
}

}  // namespace

std::vector<std::string> Exporter::buildCommand(const Project &project,
                                                const ExportRequest &request, int frameCount,
                                                const std::string &audioPathOverride,
                                                bool copyAudio, double audioSeek,
                                                bool muteAudio) {
    const OutputSpec &out = project.output;
    const std::string &audioPath = audioPathOverride.empty() ? project.audio.path
                                                            : audioPathOverride;
    std::vector<std::string> args;
    args.push_back(ffmpeg::ffmpegPath());
    args.push_back("-hide_banner");
    args.push_back("-nostdin");
    args.push_back(request.overwrite ? "-y" : "-n");
    args.push_back("-loglevel");
    args.push_back("error");
    args.push_back("-nostats");

    // ---- raw video from stdin -------------------------------------------------
    args.push_back("-f");
    args.push_back("rawvideo");
    args.push_back("-pixel_format");
    args.push_back("rgba");
    args.push_back("-video_size");
    args.push_back(std::to_string(project.video.width) + "x" +
                   std::to_string(project.video.height));
    args.push_back("-framerate");
    {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.6f", project.video.fps);
        args.push_back(buffer);
    }
    args.push_back("-i");
    args.push_back("-");

    // ---- audio ---------------------------------------------------------------
    const bool hasAudio = !muteAudio && !audioPath.empty();
    if (hasAudio) {
        const double seek = audioSeek >= 0.0 ? audioSeek : project.video.trimStart;
        if (seek > 0.0) {
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "%.6f", seek);
            args.push_back("-ss");
            args.push_back(buffer);
        }
        args.push_back("-i");
        args.push_back(audioPath);
        args.push_back("-map");
        args.push_back("0:v:0");
        args.push_back("-map");
        args.push_back("1:a:0");
    }

    // ---- video codec ---------------------------------------------------------
    args.push_back("-c:v");
    args.push_back(out.videoCodec);
    const std::string family = videoEncoderFamily(out.videoCodec);
    const std::string api = videoEncoderApi(out.videoCodec);
    if (api == "nvenc") {
        // Constant-quality VBR is the NVENC equivalent of CRF; `-b:v 0` lets the
        // quality target drive the bitrate.
        args.push_back("-preset");
        args.push_back("p5");
        if (family != "av1") {
            args.push_back("-tune");
            args.push_back("hq");
        }
        args.push_back("-rc");
        args.push_back("vbr");
        args.push_back("-cq");
        args.push_back(std::to_string(std::clamp(out.crf, 0, 51)));
        args.push_back("-b:v");
        args.push_back(out.bitrateKbps > 0 ? std::to_string(out.bitrateKbps) + "k" : "0");
    } else if (api == "qsv") {
        // ICQ: the quality target behaves like CRF.
        args.push_back("-global_quality");
        args.push_back(std::to_string(std::clamp(out.crf, 1, 51)));
        args.push_back("-look_ahead");
        args.push_back("1");
    } else if (api == "amf") {
        args.push_back("-quality");
        args.push_back("quality");
        args.push_back("-rc");
        args.push_back("cqp");
        args.push_back("-qp_i");
        args.push_back(std::to_string(std::clamp(out.crf, 1, 51)));
        args.push_back("-qp_p");
        args.push_back(std::to_string(std::clamp(out.crf, 1, 51)));
    } else if (out.videoCodec == "libvpx-vp9") {
        args.push_back("-crf");
        args.push_back(std::to_string(out.crf));
        args.push_back("-b:v");
        args.push_back(out.bitrateKbps > 0 ? std::to_string(out.bitrateKbps) + "k" : "0");
        args.push_back("-row-mt");
        args.push_back("1");
        args.push_back("-deadline");
        args.push_back("good");
        args.push_back("-cpu-used");
        args.push_back("2");
    } else if (out.videoCodec == "mpeg4") {
        args.push_back("-qscale:v");
        args.push_back(std::to_string(std::max(1, out.crf)));
    } else if (out.videoCodec == "libsvtav1") {
        // SVT presets are numbers (0 = slowest/best, 13 = fastest).
        args.push_back("-crf");
        args.push_back(std::to_string(std::clamp(out.crf, 1, 63)));
        args.push_back("-preset");
        args.push_back("8");
    } else {
        args.push_back("-crf");
        args.push_back(std::to_string(out.crf));
        args.push_back("-preset");
        args.push_back(out.preset);
    }
    // HEVC in MP4/MOV needs the hvc1 tag to play in QuickTime/Apple players.
    if (family == "hevc" && (out.container == "mp4" || out.container == "mov")) {
        args.push_back("-tag:v");
        args.push_back("hvc1");
    }
    args.push_back("-pix_fmt");
    args.push_back(out.pixelFormat);
    if (out.fastStart && (out.container == "mp4" || out.container == "mov")) {
        args.push_back("-movflags");
        args.push_back("+faststart");
    }

    // ---- audio codec ---------------------------------------------------------
    if (hasAudio) {
        if (copyAudio) {
            // The media is already the AAC stream the importer produced, so it
            // goes through untouched instead of being encoded a second time.
            args.push_back("-c:a");
            args.push_back("copy");
        } else {
            args.push_back("-c:a");
            args.push_back(out.audioCodec);
            args.push_back("-b:a");
            args.push_back(out.audioBitrate);
            // The encoder output rate is part of the project metadata; when it is
            // left unset the pipeline rate applies.
            const int audioRate = out.audioSampleRate > 0 ? out.audioSampleRate
                                                          : (project.audio.sampleRate > 0
                                                                 ? project.audio.sampleRate
                                                                 : 48000);
            args.push_back("-ar");
            args.push_back(std::to_string(audioRate));
            // Pad with silence instead of cutting the video: `-shortest` alone
            // stops the muxer at the end of a short track, which closes the
            // raw-video pipe while frames are still being written.
            args.push_back("-af");
            args.push_back("apad");
        }
        args.push_back("-shortest");
    } else {
        args.push_back("-an");
    }

    args.push_back("-frames:v");
    args.push_back(std::to_string(frameCount));
    args.push_back(request.outputPath);
    return args;
}

std::string Exporter::describeCommand(const std::vector<std::string> &arguments) {
    std::string line;
    for (size_t i = 0; i < arguments.size(); ++i) {
        if (i) line.push_back(' ');
        const std::string &argument = arguments[i];
        if (argument.find(' ') != std::string::npos) {
            line.push_back('"');
            line += argument;
            line.push_back('"');
        } else {
            line += argument;
        }
    }
    return line;
}

// Walks one DAC input back towards the ADC, allowing only transparent blocks
// (the VU/Digital Meter passes its value through untouched) in between. Returns
// the ADC output port the channel comes from, or -1 when anything that can
// change a sample sits on the way.
int passthroughSourcePort(const Graph &graph, const Node &dac, int dacPort,
                          const Node **adcOut) {
    const Node *current = &dac;
    int port = dacPort;
    const Link *link = graph.findInputLink(current->id, port);
    for (int guard = 0; link && guard < 64; ++guard) {
        const Node *source = graph.find(link->fromNode);
        if (!source) return -1;
        if (source->kind == "dbg.meter") {
            // Transparent: one Scalar in, one Scalar out, value untouched.
            if (source->inputPorts().size() != 1 || source->outputPorts().size() != 1) return -1;
            if (link->fromPort != 0) return -1;
            current = source;
            port = 0;
            link = graph.findInputLink(current->id, port);
            continue;
        }
        if (source->kind == "dsp.adc") {
            *adcOut = source;
            return link->fromPort;
        }
        return -1;  // anything else can change the signal
    }
    return -1;
}

// A Unity ADC whose channels reach a DAC untouched - directly or through
// meters - copies the imported clip bit for bit. The verification is purely
// structural: it looks at the wiring, never at where else the signal goes, so a
// Spectrum Analyzer hanging off the DAC keeps reading the rendered graph (which
// still runs live). The export can then mux the original file (and playback uses
// the decoded clip) instead of rendering and re-encoding that region.
bool isPassthroughBridge(const Project &project) {
    const Graph &graph = project.graph;
    if (project.audio.path.empty()) return false;
    const int sink = graph.audioSinkNodeId();
    const Link *sinkLink = sink > 0 ? graph.findInputLink(sink, 0) : nullptr;
    const Node *dac = sinkLink ? graph.find(sinkLink->fromNode) : nullptr;
    if (!dac || dac->kind != "dsp.dac") return false;
    const int channels = static_cast<int>(dac->inputPorts().size());
    if (channels <= 0 || channels != project.audio.channels) return false;
    const Node *adc = nullptr;
    for (int port = 0; port < channels; ++port) {
        const Node *found = nullptr;
        const int fromPort = passthroughSourcePort(graph, *dac, port, &found);
        if (!found || fromPort != port) return false;  // left to left, right to right
        if (adc && adc->id != found->id) return false;
        adc = found;
    }
    if (!adc || static_cast<int>(adc->outputPorts().size()) != channels) return false;
    if (adc->pint("mode", 0) != 0) return false;  // Unity, not RMS or Peak
    const Link *audioLink = graph.findInputLink(adc->id, 0);
    const Node *clip = audioLink ? graph.find(audioLink->fromNode) : nullptr;
    if (!clip || clip->kind != "src.audio") return false;
    // The DAC's clamp only changes the samples when the material can exceed full
    // scale; the importer records the clip's peak so this stays exact.
    if (dac->pbool("clamp", true) && project.audio.peak > 1.0f) return false;
    return true;
}

AudioRoute Exporter::audioRoute(const Project &project) {
    const int sourceId = audioOutputSource(project.graph);
    if (sourceId <= 0) return AudioRoute::Silent;
    const Node *source = project.graph.find(sourceId);
    if (!source) return AudioRoute::Silent;
    if (source->kind == "src.audio") return AudioRoute::Source;
    if (isPassthroughBridge(project)) return AudioRoute::Source;
    return AudioRoute::Processed;
}

void Exporter::resetRenderedAudio(Graph &graph) {
    for (Node &node : graph.nodes) {
        node.audioRenderOutput.reset();
        node.audioRenderStart = -1;
        node.audioRenderFrames = 0;
        node.audioRenderKey.clear();
        node.audioRenderWrite = -1;
    }
}

bool Exporter::renderOutputAudio(Project &project, const AudioPtr &audio,
                                 const AnalysisPtr &analysis, double startTime, double endTime,
                                 const std::function<void(const ExportProgress &)> &onProgress,
                                 const std::function<bool()> &shouldCancel, AudioPtr *rendered,
                                 std::string *error) {
    if (rendered) rendered->reset();
    const int sourceId = audioOutputSource(project.graph);
    const Node *source = sourceId > 0 ? project.graph.find(sourceId) : nullptr;
    if (!source || source->kind == "src.audio") return true;  // direct or silent route

    const double audioDuration = project.audio.duration;
    const double totalDuration = project.effectiveDuration(audioDuration);
    const double start = std::clamp(startTime, 0.0, std::max(0.0, totalDuration - 1e-3));
    const double end = endTime > start ? std::min(endTime, totalDuration) : totalDuration;
    const double span = std::max(1e-3, end - start);
    const double fps = std::max(1.0, project.video.fps);
    const int frameCount = std::max(1, static_cast<int>(std::ceil(span * fps)));
    const auto audioTimeFor = [&](double videoTime) {
        const double wanted = project.video.trimStart + videoTime;
        return audioDuration > 0.0 ? std::min(audioDuration, wanted) : wanted;
    };

    // The pass always starts from empty buffers so a re-render (the monitor
    // after a parameter change, an export after playback) cannot reuse stale
    // samples.
    for (Node &node : project.graph.nodes) {
        node.audioRenderOutput.reset();
        node.audioRenderStart = -1;
        node.audioRenderFrames = 0;
        node.audioRenderKey.clear();
    }
    std::vector<std::pair<int, std::unordered_map<std::string, double>>> savedState;
    savedState.reserve(project.graph.nodes.size());
    for (Node &node : project.graph.nodes) {
        savedState.emplace_back(node.id, node.runtimeState);
        node.runtimeState.clear();
    }

    EvalContext ctx;
    ctx.width = project.video.width;
    ctx.height = project.video.height;
    ctx.fps = static_cast<float>(fps);
    ctx.duration = totalDuration;
    ctx.offline = true;
    ctx.audio = audio;
    ctx.analysis = analysis;
    bool cancelled = false;
    for (int frame = 0; frame < frameCount; ++frame) {
        const double videoTime = start + static_cast<double>(frame) / fps;
        ctx.time = videoTime;
        ctx.frame = frame;
        ctx.audioTime = audioTimeFor(videoTime);
        project.graph.evaluate(ctx);
        if (shouldCancel && shouldCancel()) {
            cancelled = true;
            break;
        }
        if (onProgress && frame % 32 == 0) {
            ExportProgress progress;
            progress.frame = frame;
            progress.frameCount = frameCount;
            progress.videoTime = videoTime;
            progress.status = "audio";
            onProgress(progress);
        }
    }

    const Node *sink = project.graph.find(sourceId);
    AudioPtr result = sink ? sink->audioRenderOutput : nullptr;
    // The render pass needs the pre-pass modulation state; the rendered audio
    // buffers stay so the video pass can reuse them.
    for (const auto &entry : savedState) {
        if (Node *node = project.graph.find(entry.first)) node->runtimeState = entry.second;
    }
    if (cancelled) {
        if (error) *error = "export cancelled";
        return false;
    }
    if (!result || result->frameCount <= 0) {
        if (error) *error = "the Audio Output produced no audio";
        return false;
    }
    // `result` keeps the track alive; the nodes must not hold whole-track
    // buffers into the next preview pass.
    resetRenderedAudio(project.graph);
    if (rendered) *rendered = std::move(result);
    return true;
}

bool Exporter::run(Renderer &renderer, Project &project, const ExportRequest &request,
                   const AudioPtr &audio, const AnalysisPtr &analysis,
                   const std::function<void(const ExportProgress &)> &onProgress,
                   const std::function<bool()> &shouldCancel, std::string *error,
                   const std::vector<unsigned char> *embeddedAudio) {
    const double audioDuration = project.audio.duration;
    const double totalDuration = project.effectiveDuration(audioDuration);
    const double start = std::clamp(request.startTime, 0.0, std::max(0.0, totalDuration - 1e-3));
    const double end = request.endTime > start ? std::min(request.endTime, totalDuration)
                                               : totalDuration;
    const double span = std::max(1e-3, end - start);
    const double fps = std::max(1.0, project.video.fps);
    const int frameCount = std::max(1, static_cast<int>(std::ceil(span * fps)));
    // With no decoded media the graph can still synthesise a track (DAC), so
    // the audio clock follows the video clock instead of being pinned to 0.
    const auto audioTimeFor = [&](double videoTime) {
        const double wanted = project.video.trimStart + videoTime;
        return audioDuration > 0.0 ? std::min(audioDuration, wanted) : wanted;
    };

    if (project.output.container.empty()) {
        if (error) *error = "no output container configured";
        return false;
    }
    if (!ffmpeg::available()) {
        if (error) *error = "ffmpeg was not found; install it or set the path in the CMake cache";
        return false;
    }

    // Media the importer converted to AAC lives in memory; ffmpeg wants a file,
    // so it is written to a temporary path for the duration of the export.
    std::string audioPath = project.audio.path;
    std::string tempAudioPath;
    bool copyAudio = false;
    if (embeddedAudio && !embeddedAudio->empty() && containerAcceptsAac(project.output.container)) {
        std::error_code pathError;
        const std::filesystem::path tempDir = std::filesystem::temp_directory_path(pathError);
        if (!pathError) {
            tempAudioPath = (tempDir / "pulseforge_import_audio.aac").string();
            std::ofstream file(tempAudioPath, std::ios::binary | std::ios::trunc);
            if (file) {
                file.write(reinterpret_cast<const char *>(embeddedAudio->data()),
                           static_cast<std::streamsize>(embeddedAudio->size()));
                file.close();
                audioPath = tempAudioPath;
                // Copying only works while the track covers the video: a shorter
                // one would cut the render off early. The stream rate has to be
                // the project's rate too, otherwise it gets re-encoded.
                copyAudio = project.output.audioCodec == "aac" &&
                            project.audio.duration + 0.05 >= totalDuration &&
                            (project.output.audioSampleRate <= 0 ||
                             project.audio.transcodedRate <= 0 ||
                             project.output.audioSampleRate == project.audio.transcodedRate);
            }
        }
    }

    // ---- audio routing -------------------------------------------------------
    // Only the signal patched into the Audio Output is muxed. A direct link from
    // the Audio Source keeps the original file and its matched encoder; anything
    // else (Dynamics, DAC, a processed chain) is rendered by the graph. No Audio
    // Output, or an unconnected one, means a silent export.
    const AudioRoute route = audioRoute(project);
    if (route != AudioRoute::Source) {
        audioPath.clear();
        copyAudio = false;
    }
    // A dry export still starts with empty DSP buffers so the video pass cannot
    // reuse windows left behind by preview playback; renderOutputAudio clears
    // them itself for the processed route.
    for (Node &node : project.graph.nodes) {
        node.audioRenderOutput.reset();
        node.audioRenderStart = -1;
        node.audioRenderFrames = 0;
        node.audioRenderKey.clear();
    }

    // ---- graph audio rendering ----------------------------------------------
    // ffmpeg wants the whole track before the first video frame is written, so
    // the graph is evaluated once up front with the renderer detached. The
    // rendered buffer is written to a temporary float WAV; the video pass below
    // reuses the same node buffers.
    std::string processedAudioPath;
    double audioSeek = -1.0;
    if (route == AudioRoute::Processed) {
        std::error_code pathError;
        const std::filesystem::path tempDir = std::filesystem::temp_directory_path(pathError);
        if (pathError) {
            if (!tempAudioPath.empty()) std::filesystem::remove(tempAudioPath);
            if (error) *error = "could not locate a temporary directory for the rendered audio";
            return false;
        }
        AudioPtr rendered;
        std::string renderError;
        if (!renderOutputAudio(project, audio, analysis, start, end, onProgress, shouldCancel,
                               &rendered, &renderError)) {
            if (!tempAudioPath.empty()) std::filesystem::remove(tempAudioPath);
            if (error) *error = renderError;
            return false;
        }
        const std::filesystem::path tempPath = tempDir / "pulseforge_graph_audio.wav";
        std::string wavError;
        if (!writeFloatWav(tempPath, *rendered, 0, rendered->frameCount, &wavError)) {
            std::filesystem::remove(tempPath);
            if (!tempAudioPath.empty()) std::filesystem::remove(tempAudioPath);
            if (error) {
                *error = "the Audio Output produced no audio";
                if (!wavError.empty()) *error += ": " + wavError;
            }
            return false;
        }
        processedAudioPath = tempPath.string();
        audioPath = processedAudioPath;
        copyAudio = false;
        audioSeek = 0.0;  // the rendered clip already starts at the export offset
    }
    const std::vector<std::string> arguments = buildCommand(
        project, request, frameCount, audioPath, copyAudio, audioSeek,
        route == AudioRoute::Silent);
    ChildProcess encoder;
    std::string startError;
    if (!encoder.start(arguments, &startError)) {
        if (error) *error = "could not start ffmpeg: " + startError;
        if (!tempAudioPath.empty()) std::filesystem::remove(tempAudioPath);
        if (!processedAudioPath.empty()) std::filesystem::remove(processedAudioPath);
        return false;
    }
    FrameReadback readback;
    EvalContext ctx;
    ctx.width = project.video.width;
    ctx.height = project.video.height;
    ctx.fps = static_cast<float>(fps);
    ctx.duration = totalDuration;
    ctx.offline = true;
    ctx.audio = audio;
    ctx.analysis = analysis;

    const double wallStart = GetTime();
    double totalRenderMs = 0.0;
    double totalEncodeMs = 0.0;
    bool ok = true;
    std::string failure;

    for (int frame = 0; frame < frameCount; ++frame) {
        const double videoTime = start + static_cast<double>(frame) / fps;
        ctx.time = videoTime;
        ctx.frame = frame;
        ctx.audioTime = audioTimeFor(videoTime);

        std::string renderError;
        ImageBufferPtr image = renderer.renderFrame(project.graph, ctx, &renderError);
        totalRenderMs += renderer.stats().frameMs;
        if (!image) {
            ok = false;
            failure = renderError.empty() ? "the pipeline produced no image" : renderError;
            break;
        }
        readback.capture(image->texture);

        if (frame > 0) {
            if (const unsigned char *pixels = readback.fetch()) {
                const double encodeStart = GetTime();
                if (!encoder.write(pixels, readback.byteSize())) {
                    ok = false;
                    // Drain the encoder so its message is complete before it is
                    // reported: ffmpeg prints the reason on exit.
                    encoder.closeStdin();
                    int exitCode = 0;
                    encoder.wait(&exitCode);
                    const std::string detail = encoder.stderrText();
                    failure = "ffmpeg stopped accepting frames (exit code " +
                              std::to_string(exitCode) + ")" +
                              (detail.empty() ? std::string() : ": " + detail);
                }
                totalEncodeMs += (GetTime() - encodeStart) * 1000.0;
                readback.release();
            }
            if (!ok) break;
        }

        if (onProgress) {
            ExportProgress progress;
            progress.frame = frame + 1;
            progress.frameCount = frameCount;
            progress.videoTime = videoTime;
            progress.elapsed = GetTime() - wallStart;
            progress.renderMs = renderer.stats().frameMs;
            progress.encodeMs = totalEncodeMs / std::max(1, frame);
            progress.fps = progress.elapsed > 0.0 ? (frame + 1) / progress.elapsed : 0.0;
            const double perFrame = progress.elapsed / std::max(1, frame + 1);
            progress.remaining = perFrame * (frameCount - frame - 1);
            progress.status = "encoding";
            onProgress(progress);
        }
        if (shouldCancel && shouldCancel()) {
            ok = false;
            failure = "export cancelled";
            break;
        }
    }

    if (ok) {
        // Flush the last frame still held in the PBO ring.
        if (ImageBufferPtr image = renderer.outputTarget()) {
            readback.capture(image->texture);
            if (const unsigned char *pixels = readback.fetch()) {
                if (!encoder.write(pixels, readback.byteSize())) {
                    ok = false;
                    failure = "ffmpeg stopped accepting frames: " + encoder.stderrText();
                }
                readback.release();
            }
        }
    }

    readback.release();
    readback.shutdown();
    encoder.closeStdin();
    encoder.wait();
    if (!tempAudioPath.empty()) std::filesystem::remove(tempAudioPath);
    if (!processedAudioPath.empty()) std::filesystem::remove(processedAudioPath);
    if (ok && !encoder.stderrText().empty()) {
        // Warnings only: ffmpeg returned success.
    }
    if (!ok) {
        if (error) *error = failure;
        return false;
    }

    if (onProgress) {
        ExportProgress progress;
        progress.frame = frameCount;
        progress.frameCount = frameCount;
        progress.elapsed = GetTime() - wallStart;
        progress.fps = progress.elapsed > 0.0 ? frameCount / progress.elapsed : 0.0;
        progress.renderMs = totalRenderMs / std::max(1, frameCount);
        progress.encodeMs = totalEncodeMs / std::max(1, frameCount);
        progress.status = "done";
        onProgress(progress);
    }
    // The export's passes left every node holding a buffer that spans the whole
    // track; the preview's reuse check would accept those stale windows and the
    // monitor would never be handed a new sample again - the app went silent
    // after an export. Playback now starts from fresh windows.
    resetRenderedAudio(project.graph);
    return true;
}

}  // namespace pf
