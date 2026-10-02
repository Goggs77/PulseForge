#include "export/Exporter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "export/FFmpeg.h"
#include "raylib.h"
#include "render/FrameReadback.h"

namespace pf {

std::vector<std::string> Exporter::buildCommand(const Project &project,
                                                const ExportRequest &request, int frameCount,
                                                const std::string &audioPathOverride,
                                                bool copyAudio) {
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
    const bool hasAudio = !audioPath.empty();
    if (hasAudio) {
        if (project.video.trimStart > 0.0) {
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "%.6f", project.video.trimStart);
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
    const std::vector<std::string> arguments =
        buildCommand(project, request, frameCount, audioPath, copyAudio);
    ChildProcess encoder;
    std::string startError;
    if (!encoder.start(arguments, &startError)) {
        if (error) *error = "could not start ffmpeg: " + startError;
        if (!tempAudioPath.empty()) std::filesystem::remove(tempAudioPath);
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
        ctx.audioTime = std::min(audioDuration, project.video.trimStart + videoTime);

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
    return true;
}

}  // namespace pf
