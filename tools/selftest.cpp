// PulseForge end-to-end self test.
//
//   pf_selftest [audio-file] [output-video] [seconds]
//
// When no audio file is given a synthetic drum-and-sweep WAV is generated so
// the test always has something to analyse. The test renders the default
// pipeline into a hidden window, exports a real video through ffmpeg and then
// probes the result.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "core/Project.h"
#include "core/Registry.h"
#include "dsp/Analysis.h"
#include "dsp/AudioClip.h"
#include "export/Exporter.h"
#include "export/FFmpeg.h"
#include "raylib.h"
#include "render/Renderer.h"

using namespace pf;

namespace {

// ---------------------------------------------------------------------------
// A small 16-bit PCM WAV writer used to synthesise test material.
// ---------------------------------------------------------------------------
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

bool generateTestWav(const std::string &path, double seconds, int sampleRate) {
    const int frames = static_cast<int>(seconds * sampleRate);
    std::vector<float> samples(static_cast<size_t>(frames), 0.0f);
    const double bpm = 124.0;
    const double beat = 60.0 / bpm;
    for (int i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / sampleRate;
        // kick: decaying sine with a pitch drop
        const double beatPhase = std::fmod(t, beat);
        const double kickEnv = std::exp(-beatPhase * 18.0);
        const double kickFreq = 120.0 * std::exp(-beatPhase * 12.0) + 45.0;
        double value = std::sin(6.2831853 * kickFreq * beatPhase) * kickEnv * 0.75;
        // hat on the off beat
        const double offPhase = std::fmod(t + beat * 0.5, beat);
        if (offPhase < 0.05) {
            value += (static_cast<double>(rand()) / RAND_MAX - 0.5) *
                     std::exp(-offPhase * 90.0) * 0.22;
        }
        // sweeping lead
        const double sweep = 220.0 * std::pow(2.0, 2.6 * std::fmod(t, 4.0) / 4.0);
        value += std::sin(6.2831853 * sweep * t) * 0.16 * (0.6 + 0.4 * std::sin(t * 1.7));
        // pad
        value += std::sin(6.2831853 * 82.41 * t) * 0.10;
        samples[static_cast<size_t>(i)] = static_cast<float>(std::clamp(value, -1.0, 1.0));
    }

    std::ofstream out(path.c_str(), std::ios::binary);
    if (!out.good()) return false;
    const unsigned int dataBytes = static_cast<unsigned int>(samples.size() * 2);
    out.write("RIFF", 4);
    writeLE32(out, 36 + dataBytes);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    writeLE32(out, 16);
    writeLE16(out, 1);        // PCM
    writeLE16(out, 1);        // mono
    writeLE32(out, static_cast<unsigned int>(sampleRate));
    writeLE32(out, static_cast<unsigned int>(sampleRate * 2));
    writeLE16(out, 2);        // block align
    writeLE16(out, 16);       // bits
    out.write("data", 4);
    writeLE32(out, dataBytes);
    for (float sample : samples) {
        const short value = static_cast<short>(std::lround(sample * 32000.0f));
        writeLE16(out, static_cast<unsigned short>(value));
    }
    return true;
}

int fail(const std::string &message) {
    std::printf("FAIL: %s\n", message.c_str());
    return 1;
}

}  // namespace

int main(int argc, char **argv) {
    const std::string audioPath = argc > 1 ? argv[1] : std::string("selftest_input.wav");
    const std::string outputPath = argc > 2 ? argv[2] : std::string("selftest_output.mp4");
    const double seconds = argc > 3 ? std::atof(argv[3]) : 4.0;
    const int width = argc > 4 ? std::atoi(argv[4]) : 640;
    const int height = argc > 5 ? std::atoi(argv[5]) : 360;

    std::printf("PulseForge self test\n");
    std::printf("  ffmpeg   : %s\n", ffmpeg::ffmpegPath().c_str());
    std::printf("  ffprobe  : %s\n", ffmpeg::ffprobePath().c_str());

    {
        std::ifstream probe(audioPath.c_str(), std::ios::binary);
        if (probe.good()) {
            probe.close();
        } else {
            if (!generateTestWav(audioPath, seconds + 1.0, 48000)) {
                return fail("could not write the synthetic test wav");
            }
            std::printf("  generated: %s\n", audioPath.c_str());
        }
    }

    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(320, 240, "PulseForge self test");
    if (!IsWindowReady()) return fail("no window / GL context");
    InitAudioDevice();

    int result = 0;
    Renderer renderer;
    std::string error;
    if (!renderer.init(&error)) {
        result = fail("renderer init: " + error);
    }

    if (result == 0) {
        AudioClip clip;
        if (!clip.load(audioPath, 48000, &error)) {
            result = fail("audio decode: " + error);
        } else {
            std::printf("  audio    : %.2f s, %d ch @ %d Hz\n", clip.duration(), clip.channels(),
                        clip.sampleRate());
            AnalysisPtr analysis = analyzeAudio(*clip.buffer(), AnalysisSettings{});
            std::printf("  analysis : %zu frames in %.2f s (%.1f ms/frame), %d bands\n",
                        analysis->frames.size(), analysis->computeSeconds,
                        analysis->frames.empty()
                            ? 0.0
                            : analysis->computeSeconds * 1000.0 / analysis->frames.size(),
                        analysis->bandCount);

            Project project;
            project.resetToDefault();
            project.name = "Self test";
            project.video.width = width;
            project.video.height = height;
            project.video.fps = 30.0;
            project.video.useAudioDuration = false;
            project.video.duration = seconds;
            project.output = outputSpecForContainer("mp4");
            project.audio.path = audioPath;
            project.audio.duration = clip.duration();
            project.audio.sampleRate = clip.sampleRate();
            project.audio.channels = clip.channels();

            // --- render a few frames in-process to measure throughput -----
            RenderStats stats{};
            const int previewFrames = 12;
            for (int i = 0; i < previewFrames && result == 0; ++i) {
                EvalContext ctx;
                ctx.width = width;
                ctx.height = height;
                ctx.fps = 30.0f;
                ctx.duration = clip.duration();
                ctx.time = i / 30.0;
                ctx.frame = i;
                ctx.audioTime = ctx.time;
                ctx.audio = clip.buffer();
                ctx.analysis = analysis;
                std::string renderError;
                if (!renderer.renderFrame(project.graph, ctx, &renderError)) {
                    result = fail("render frame " + std::to_string(i) + ": " + renderError);
                }
                stats = renderer.stats();
            }
            if (result == 0) {
                std::printf("  render   : %.2f ms/frame, %d shader passes, %d pooled targets\n",
                            stats.frameMs, stats.shaderPasses, stats.pooledTargets);
            }

            // --- project round trip ------------------------------------
            if (result == 0) {
                const std::string projectPath = "selftest_project.pforge";
                std::string saveError;
                const int blocksBefore = project.graph.nodeCount();
                const int linksBefore = static_cast<int>(project.graph.links.size());
                if (!project.save(projectPath, &saveError)) {
                    result = fail("project save: " + saveError);
                } else {
                    Project reloaded;
                    if (!reloaded.load(projectPath, &saveError)) {
                        result = fail("project load: " + saveError);
                    } else {
                        const int blocksAfter = reloaded.graph.nodeCount();
                        const int linksAfter = static_cast<int>(reloaded.graph.links.size());
                        const bool metadataOk = reloaded.video.width == project.video.width &&
                                                reloaded.video.height == project.video.height &&
                                                std::fabs(reloaded.video.fps - project.video.fps) < 1e-6;
                        const bool mediaOk = reloaded.audio.path == project.audio.path;
                        std::printf("  project  : %d/%d blocks, %d/%d links, metadata %s, media %s\n",
                                    blocksAfter, blocksBefore, linksAfter, linksBefore,
                                    metadataOk ? "ok" : "MISMATCH", mediaOk ? "ok" : "MISMATCH");
                        if (blocksAfter != blocksBefore || linksAfter != linksBefore || !metadataOk ||
                            !mediaOk) {
                            result = fail("project round trip mismatch");
                        }
                    }
                }
            }

            // --- math blocks -------------------------------------------
            if (result == 0) {
                Graph math;
                Node *constant = math.addNode("math.constant", 0, 0);
                Node *arith = math.addNode("math.arithmetic", 0, 0);
                Node *power = math.addNode("math.power", 0, 0);
                Node *expo = math.addNode("math.exp", 0, 0);
                Node *logar = math.addNode("math.log", 0, 0);
                Node *trig = math.addNode("math.trig", 0, 0);
                Node *hyper = math.addNode("math.hyperbolic", 0, 0);
                Node *invtrig = math.addNode("math.inverse_trig", 0, 0);
                Node *vec2 = math.addNode("math.vec2", 0, 0);
                Node *vec3 = math.addNode("math.vec3", 0, 0);
                Node *vec4 = math.addNode("math.vec4", 0, 0);
                Node *matrix = math.addNode("math.matrix", 0, 0);
                Node *determinant = math.addNode("math.determinant", 0, 0);
                if (!constant || !arith || !power || !expo || !logar || !trig || !hyper ||
                    !invtrig || !vec2 || !vec3 || !vec4 || !matrix || !determinant) {
                    result = fail("could not create the math blocks");
                } else {
                    constant->setFloat("value", 0.25f);
                    trig->setInt("unit", 1);          // turns
                    trig->setFloat("frequency", 1.0f);
                    vec2->setFloat("y", 0.0f);
                    vec3->setFloat("z", 0.0f);
                    vec4->setFloat("w", 0.0f);
                    matrix->setInt("size", 1);        // 3x3
                    determinant->setInt("size", 1);   // 3x3
                    if (Param *grid = matrix->find("matrix")) {
                        // Make the 3x3 determinant differ from the 2x2 one so the
                        // size mapping is actually exercised.
                        grid->values[10] = 2.0f;
                    }

                    // Every connection is asserted: a rejected link would
                   // constant -> power -> exp -> log -> trig -> hyperbolic -> inverse trig
                    // quietly leave a block with default inputs.
                    auto link = [&](Node *from, int fromPort, Node *to, int toPort) {
                        std::string why;
                        if (!math.connect(from->id, fromPort, to->id, toPort, &why)) {
                            std::printf("    link %s[%d]:%s -> %s[%d]:%s rejected: %s\n",
                                        from->kind.c_str(), fromPort,
                                        from->def->outputs[static_cast<size_t>(fromPort)].name.c_str(),
                                        to->kind.c_str(), toPort,
                                        to->def->inputs[static_cast<size_t>(toPort)].name.c_str(),
                                        why.c_str());
                            result = fail("math connection rejected: " + why);
                            return false;
                        }
                        return true;
                    };
                    link(constant, 0, arith, 0);
                    link(constant, 0, power, 0);
                    link(arith, 0, expo, 0);
                    link(expo, 0, logar, 0);
                    link(constant, 0, trig, 0);
                    link(trig, 0, hyper, 0);
                    link(hyper, 2, invtrig, 0);   // tanh
                    link(power, 0, vec2, 0);
                    link(trig, 0, vec2, 1);       // sin, so v2.y is 1 at 0.25 turns
                    link(vec2, 1, vec3, 0);       // X, Y and Z are separate scalars
                    link(vec2, 2, vec3, 1);
                    link(vec3, 1, vec4, 0);
                    link(vec3, 2, vec4, 1);
                    link(vec3, 3, vec4, 2);
                    link(vec4, 0, matrix, 0);     // Vector4 -> matrix row
                    link(matrix, 0, determinant, 0);

                    EvalContext mathContext;
                    mathContext.duration = 4.0;
                    mathContext.fps = 30.0f;
                    mathContext.time = 1.0;
                    if (!math.evaluate(mathContext)) {
                        result = fail("math graph did not evaluate: " + math.lastError);
                    } else {
                        const float sine = trig->outputs[0].scalar;
                        const float cosine = trig->outputs[1].scalar;
                        const float tanhValue = hyper->outputs[2].scalar;
                        const float determinantValue = determinant->outputs[0].scalar;
                        const Vector2 v2 = vec2->outputs[0].vec2;
                        const Vector3 v3 = vec3->outputs[0].vec3;
                        const Vector4 v4 = vec4->outputs[0].vec4;
                        std::printf("  math     : sin=%.4f cos=%.4f tanh=%.4f det=%.3f "
                                    "v2=(%.3f,%.3f) v3.z=%.3f v4.w=%.3f\n",
                                    sine, cosine, tanhValue, determinantValue, v2.x, v2.y, v3.z, v4.w);
                        const Matrix &mm = matrix->outputs[0].matrix;
                        std::printf("  matrix   : [%.4f %.4f %.4f %.4f][%.4f %.4f %.4f %.4f]"
                                    "[%.4f %.4f %.4f %.4f][%.4f %.4f %.4f %.4f]\n",
                                    mm.m0, mm.m1, mm.m2, mm.m3, mm.m4, mm.m5, mm.m6, mm.m7, mm.m8,
                                    mm.m9, mm.m10, mm.m11, mm.m12, mm.m13, mm.m14, mm.m15);
                        const bool trigOk = std::fabs(sine - 1.0f) < 1e-3f &&
                                            std::fabs(cosine) < 1e-3f;
                        const bool tanhOk = std::fabs(tanhValue - std::tanh(sine)) < 1e-3f;
                        const bool vectorOk = std::fabs(v2.x - 0.0625f) < 1e-4f &&
                                              std::fabs(v2.y - 1.0f) < 1e-3f &&
                                              std::fabs(v4.w) < 1e-6f;
                        // The 4x4 has the vector in row 0 and the identity below,
                        // so its determinant is the element (0,0).
                        const bool determinantOk = std::fabs(determinantValue - 0.125f) < 1e-3f;
                        if (!trigOk || !tanhOk || !vectorOk || !determinantOk) {
                            result = fail("math block outputs are wrong");
                        }
                    }
                }
            }

            // --- export the real thing ------------------------------------
            if (result == 0) {
                ExportRequest request;
                request.outputPath = outputPath;
                request.overwrite = true;
                double lastPrinted = -1.0;
                const bool ok = Exporter::run(
                    renderer, project, request, clip.buffer(), analysis,
                    [&](const ExportProgress &progress) {
                        if (progress.elapsed - lastPrinted >= 0.5 ||
                            progress.frame == progress.frameCount) {
                            lastPrinted = progress.elapsed;
                            std::printf(
                                "    %4d/%-4d %5.1f%%  %.1f fps  eta %4.1fs  (render %.1f ms)\n",
                                progress.frame, progress.frameCount,
                                100.0 * progress.frame / std::max(1, progress.frameCount),
                                progress.fps, progress.remaining, progress.renderMs);
                            std::fflush(stdout);
                        }
                    },
                    []() { return false; }, &error);
                if (!ok) {
                    result = fail("export: " + error);
                } else {
                    const MediaInfo info = ffmpeg::probe(outputPath);
                    if (!info.ok) {
                        result = fail("exported file could not be probed");
                    } else {
                        std::printf("  export   : %s  %.2f s  format=%s  audio=%s\n",
                                    outputPath.c_str(), info.duration, info.format.c_str(),
                                    info.codec.c_str());
                    }
                }
            }
            clip.clear();
        }
    }
    renderer.shutdown();

    CloseAudioDevice();
    CloseWindow();
    std::printf(result == 0 ? "self test OK\n" : "self test FAILED\n");
    return result;
}
