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
#include <limits>
#include <string>
#include <vector>

#include "core/Project.h"
#include "core/Registry.h"
#include "core/Sorting.h"
#include "core/TextEdit.h"
#include "dsp/Analysis.h"
#include "dsp/AudioClip.h"
#include "dsp/Dynamics.h"
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

// 24-bit BMP with the top half red and the bottom half blue (rows are stored
// bottom-up, so the blue rows go first). Used by the Picture block test: the
// two colours also prove the image is drawn the right way up.
bool writePictureBmp(const std::string &path, int width, int height) {
    std::ofstream out(path.c_str(), std::ios::binary);
    if (!out.good()) return false;
    const int stride = (width * 3 + 3) & ~3;
    const unsigned int imageBytes = static_cast<unsigned int>(stride) * height;
    out.write("BM", 2);
    writeLE32(out, 54u + imageBytes);
    writeLE32(out, 0u);
    writeLE32(out, 54u);
    writeLE32(out, 40u);
    writeLE32(out, static_cast<unsigned int>(width));
    writeLE32(out, static_cast<unsigned int>(height));
    writeLE16(out, 1);
    writeLE16(out, 24);
    writeLE32(out, 0u);
    writeLE32(out, imageBytes);
    writeLE32(out, 2835u);
    writeLE32(out, 2835u);
    writeLE32(out, 0u);
    writeLE32(out, 0u);
    std::vector<unsigned char> row(static_cast<size_t>(stride), 0);
    for (int y = 0; y < height; ++y) {
        // Stored bottom-up: the first stored rows are the picture's bottom.
        const bool top = y >= height / 2;
        for (int x = 0; x < width; ++x) {
            row[static_cast<size_t>(x) * 3 + 0] = top ? 0 : 255;  // B
            row[static_cast<size_t>(x) * 3 + 1] = 0;              // G
            row[static_cast<size_t>(x) * 3 + 2] = top ? 255 : 0;  // R
        }
        out.write(reinterpret_cast<const char *>(row.data()), stride);
    }
    return out.good();
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

// A pure sine on disk, used where the exporter has to read the media file
// itself (the dry Audio Output path) instead of the in-memory buffer.
bool writeSineWav(const std::string &path, double seconds, int sampleRate, float amplitude) {
    const int frames = static_cast<int>(seconds * sampleRate);
    std::ofstream out(path.c_str(), std::ios::binary);
    if (!out.good()) return false;
    const unsigned int dataBytes = static_cast<unsigned int>(frames * 4);
    out.write("RIFF", 4);
    writeLE32(out, 36 + dataBytes);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    writeLE32(out, 16);
    writeLE16(out, 1);  // PCM
    writeLE16(out, 2);  // stereo, so decoding to 2 channels does not rescale the level
    writeLE32(out, static_cast<unsigned int>(sampleRate));
    writeLE32(out, static_cast<unsigned int>(sampleRate * 4));
    writeLE16(out, 4);
    writeLE16(out, 16);
    out.write("data", 4);
    writeLE32(out, dataBytes);
    for (int i = 0; i < frames; ++i) {
        const float sample =
            amplitude * std::sin(6.2831853f * 100.0f * static_cast<float>(i) / sampleRate);
        const unsigned short value = static_cast<unsigned short>(
            static_cast<short>(std::lround(sample * 32767.0f)));
        writeLE16(out, value);
        writeLE16(out, value);
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
            AnalysisPtr analysis =
                analyzeAudio(*clip.buffer(), AnalysisSettings{}, {}, clip.buffer());
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

            // Modulating Spectrum's Scale must not allocate a target per frame:
            // the scale is quantised, feedback stays at the base resolution and
            // the renderer pool is capped.
            if (result == 0) {
                Graph scaleGraph;
                Node *src = scaleGraph.addNode("src.audio", 0, 0);
                Node *analyzer = scaleGraph.addNode("dsp.analyze", 200, 0);
                Node *scaleSource = scaleGraph.addNode("math.constant", 200, 140);
                Node *spectrum = scaleGraph.addNode("render.spectrum", 420, 0);
                Node *output = scaleGraph.addNode("out.video", 700, 0);
                bool scaleOk = src && analyzer && scaleSource && spectrum && output;
                std::string scaleWhat;
                if (!scaleOk) {
                    scaleWhat = "blocks missing";
                } else {
                    spectrum->setInt("preset", 4);  // radial_spectrum
                    spectrum->setBool("useFeedback", true);
                    spectrum->setFloat("feedback", 0.4f);
                    std::string why;
                    scaleOk = scaleGraph.connect(src->id, 0, analyzer->id, 0, &why) &&
                              scaleGraph.connect(analyzer->id, 0, spectrum->id, 0, &why) &&
                              scaleGraph.connect(scaleSource->id, 0, spectrum->id, 1, &why) &&
                              scaleGraph.connect(spectrum->id, 0, output->id, 0, &why);
                    if (!scaleOk) scaleWhat = why;
                }
                if (scaleOk) {
                    EvalContext ctx;
                    ctx.width = 320;
                    ctx.height = 180;
                    ctx.fps = 60.0f;
                    ctx.duration = 2.0;
                    ctx.audio = clip.buffer();
                    ctx.analysis = analysis;
                    const double started = GetTime();
                    for (int frame = 0; frame < 120; ++frame) {
                        const float phase = static_cast<float>(frame) / 120.0f;
                        scaleSource->setFloat("value", std::sin(phase * 6.2831853f) * 0.5f);
                        ctx.frame = frame;
                        ctx.time = static_cast<double>(frame) / 60.0;
                        ctx.audioTime = ctx.time;
                        std::string renderError;
                        renderer.renderFrame(scaleGraph, ctx, &renderError);
                    }
                    const int pooled = renderer.stats().pooledTargets;
                    if (pooled > 12) {
                        scaleOk = false;
                        scaleWhat = "target pool grew to " + std::to_string(pooled);
                    } else {
                        std::printf("  spectrum : Scale modulation stable (%d pooled targets, "
                                    "%.1f ms)\n",
                                    pooled, (GetTime() - started) * 1000.0);
                    }
                }
                if (!scaleOk) result = fail("Spectrum Scale modulation: " + scaleWhat);
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
                        const int audioOutId = reloaded.graph.audioSinkNodeId();
                        const Link *audioLink =
                            audioOutId ? reloaded.graph.findInputLink(audioOutId, 0) : nullptr;
                        const bool audioOutOk =
                            audioOutId != 0 && audioLink &&
                            reloaded.graph.find(audioLink->fromNode) != nullptr;
                        std::printf("  project  : %d/%d blocks, %d/%d links, metadata %s, media %s, "
                                    "audio out %s\n",
                                    blocksAfter, blocksBefore, linksAfter, linksBefore,
                                    metadataOk ? "ok" : "MISMATCH", mediaOk ? "ok" : "MISMATCH",
                                    audioOutOk ? "ok" : "MISMATCH");
                        if (blocksAfter != blocksBefore || linksAfter != linksBefore || !metadataOk ||
                            !mediaOk || !audioOutOk) {
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

            // Lerp mixes two Scalars and Clamp restricts one. Both fall back to
            // their Inspector values when a port is unconnected and let a
            // connected port take over, so they work as constants, as mixers
            // and as modulation targets at the same time.
            if (result == 0) {
                bool ok = true;
                std::string what;
                Graph scalars;
                Node *lerp = scalars.addNode("math.lerp", 0, 0);
                Node *clamp = scalars.addNode("math.clamp", 220, 0);
                Node *defClamp = scalars.addNode("math.clamp", 440, 0);
                Node *factor = scalars.addNode("math.constant", 220, 140);
                Node *value = scalars.addNode("math.constant", 440, 140);
                Node *lowLimit = scalars.addNode("math.constant", 440, 260);
                Node *highLimit = scalars.addNode("math.constant", 440, 380);
                if (!lerp || !clamp || !defClamp || !factor || !value || !lowLimit ||
                    !highLimit) {
                    ok = false;
                    what = "the Lerp/Clamp blocks are not registered";
                } else {
                    std::string why;
                    ok = scalars.connect(factor->id, 0, lerp->id, 2, &why) &&
                         scalars.connect(value->id, 0, clamp->id, 0, &why) &&
                         scalars.connect(lowLimit->id, 0, clamp->id, 1, &why) &&
                         scalars.connect(highLimit->id, 0, clamp->id, 2, &why);
                    if (!ok) what = why;
                }
                if (ok) {
                    // A and B stay on their defaults (0 and 1); Factor comes
                    // from the port.
                    factor->setFloat("value", 0.25f);
                    value->setFloat("value", 2.0f);
                    lowLimit->setFloat("value", 0.25f);
                    highLimit->setFloat("value", 1.0f);
                    EvalContext ctx;
                    ctx.duration = 1.0;
                    ctx.fps = 60.0f;
                    scalars.evaluate(ctx);
                    const float mixed = lerp->outputs[0].scalar;
                    const float limited = clamp->outputs[0].scalar;
                    // Clamp's inputs are unconnected here, so it uses its
                    // Inspector value (5) against the default -1..1 range.
                    defClamp->setFloat("value", 5.0f);
                    EvalContext defaults;
                    defaults.duration = 1.0;
                    defaults.fps = 60.0f;
                    scalars.evaluate(defaults);
                    const float defaultValue = defClamp->outputs[0].scalar;
                    // Reversed bounds still describe a range.
                    highLimit->setFloat("value", 0.5f);
                    lowLimit->setFloat("value", 1.5f);
                    EvalContext reversed;
                    reversed.duration = 1.0;
                    reversed.fps = 60.0f;
                    scalars.evaluate(reversed);
                    const float reversedValue = clamp->outputs[0].scalar;
                    // Disconnecting the Factor port hands the value back to the
                    // Inspector, which is the fallback both blocks use.
                    scalars.disconnectInput(lerp->id, 2);
                    lerp->setFloat("factor", 0.75f);
                    EvalContext fallback;
                    fallback.duration = 1.0;
                    fallback.fps = 60.0f;
                    scalars.evaluate(fallback);
                    const float fallbackValue = lerp->outputs[0].scalar;
                    if (std::fabs(mixed - 0.25f) > 1e-4f ||
                        std::fabs(limited - 1.0f) > 1e-4f ||
                        std::fabs(defaultValue - 1.0f) > 1e-4f ||
                        std::fabs(reversedValue - 1.5f) > 1e-4f ||
                        std::fabs(fallbackValue - 0.75f) > 1e-4f) {
                        ok = false;
                        what = "Lerp/Clamp values are wrong (mix " + std::to_string(mixed) +
                               ", clamp " + std::to_string(limited) + ", default " +
                               std::to_string(defaultValue) + ", reversed " +
                               std::to_string(reversedValue) + ", fallback " +
                               std::to_string(fallbackValue) + ")";
                    }
                }
                if (!ok) {
                    result = fail("Lerp/Clamp blocks: " + what);
                } else {
                    std::printf("  math     : Lerp mixes, Clamp restricts (ports and "
                                "Inspector fallbacks)\n");
                }
            }

            // Sorting: the Sticky Note and Group blocks carry no ports, the
            // group's members are stored as a text parameter, and the layered
            // order follows the chain (compacted depths, rows sorted by depth,
            // connected ports and id).
            if (result == 0) {
                bool ok = true;
                std::string what;
                Graph sorting;
                Node *sticky = sorting.addNode("sort.sticky", 0, 0);
                Node *group = sorting.addNode("sort.group", 0, 0);
                if (!sticky || !group) {
                    ok = false;
                    what = "the Sorting blocks are not registered";
                } else if (!sticky->inputPorts().empty() || !sticky->outputPorts().empty() ||
                           !group->inputPorts().empty() || !group->outputPorts().empty()) {
                    ok = false;
                    what = "Sorting blocks must not have ports";
                }
                if (ok) {
                    // 1 -> 2 -> 3 (2 is not a member, so 3 keeps depth 2) and a
                    // dangling constant 4. Member 4 has no links at all, so it
                    // sorts above 1 in the same column (fewer connected ports).
                    Node *constant = sorting.addNode("math.constant", 0, 0);
                    Node *arith = sorting.addNode("math.arithmetic", 0, 0);
                    Node *power = sorting.addNode("math.power", 0, 0);
                    Node *dangling = sorting.addNode("math.constant", 0, 0);
                    if (!constant || !arith || !power || !dangling) {
                        ok = false;
                        what = "could not build the layout graph";
                    } else {
                        std::string why;
                        ok = sorting.connect(constant->id, 0, arith->id, 0, &why) &&
                             sorting.connect(arith->id, 0, power->id, 0, &why);
                        if (!ok) what = why;
                    }
                    if (ok) {
                        const std::vector<int> members = {constant->id, power->id, dangling->id};
                        const std::vector<GroupSlot> slots = groupLayerOrder(sorting, members);
                        // Depth 2 compacts to the second column; the extra depth 1
                        // that no member uses must not leave a hole.
                        const bool layoutOk =
                            slots.size() == 3 && slots[0].id == dangling->id &&
                            slots[0].column == 0 && slots[0].row == 0 &&
                            slots[1].id == constant->id && slots[1].column == 0 &&
                            slots[1].row == 1 && slots[2].id == power->id &&
                            slots[2].column == 1 && slots[2].row == 0;
                        if (!layoutOk) {
                            ok = false;
                            what = "the layered order is wrong";
                        } else {
                            // Depth tolerance merges consecutive depths into one
                            // layer without letting the merge cascade: with 1,
                            // depth 0 and 1 share the first column and depth 2
                            // starts the second.
                            const std::vector<GroupSlot> merged = groupLayerOrder(
                                sorting, {constant->id, arith->id, power->id, dangling->id}, 1);
                            const bool mergedOk =
                                merged.size() == 4 && merged[0].id == dangling->id &&
                                merged[0].column == 0 && merged[0].row == 0 &&
                                merged[1].id == constant->id && merged[1].column == 0 &&
                                merged[1].row == 1 && merged[2].id == arith->id &&
                                merged[2].column == 0 && merged[2].row == 2 &&
                                merged[3].id == power->id && merged[3].column == 1 &&
                                merged[3].row == 0;
                            if (!mergedOk) {
                                ok = false;
                                what = "the depth tolerance did not merge the layers";
                            }
                        }
                    }
                    if (ok) {
                        // Member text survives parsing duplicates, separators and
                        // the round trip through the parameter.
                        const std::string text = " 7, 9;7, 12 ";
                        const std::vector<int> parsed = parseGroupMembers(text);
                        if (parsed.size() != 3 || parsed[0] != 7 || parsed[1] != 9 ||
                            parsed[2] != 12 ||
                            formatGroupMembers(parsed) != "7,9,12") {
                            ok = false;
                            what = "member parsing is wrong";
                        } else {
                            group->setText("members", formatGroupMembers(parsed));
                            sticky->setText("text", "note text");
                            if (group->pstr("members") != "7,9,12" ||
                                sticky->pstr("text") != "note text") {
                                ok = false;
                                what = "Sorting parameters do not round trip";
                            }
                        }
                    }
                    if (ok) {
                        // Both blocks and their text survive a save and load.
                        Project project;
                        project.graph.clear();
                        project.name = "Sorting";
                        const int groupId = project.graph.addNode("sort.group", 10.0f, 20.0f)->id;
                        const int stickyId = project.graph.addNode("sort.sticky", 10.0f, 200.0f)->id;
                        project.graph.find(groupId)->setText("members", "7,9,12");
                        project.graph.find(stickyId)->setText("text", "note text");
                        const std::string path = "selftest_sorting.pforge";
                        std::string saveError;
                        if (!project.save(path, &saveError)) {
                            ok = false;
                            what = "sorting project save: " + saveError;
                        } else {
                            Project loaded;
                            std::string loadError;
                            if (!loaded.load(path, &loadError, &renderer.shaders())) {
                                ok = false;
                                what = "sorting project load: " + loadError;
                            } else {
                                const Node *loadedGroup = loaded.graph.find(groupId);
                                const Node *loadedSticky = loaded.graph.find(stickyId);
                                if (!loadedGroup || !loadedSticky ||
                                    loadedGroup->pstr("members") != "7,9,12" ||
                                    loadedSticky->pstr("text") != "note text") {
                                    ok = false;
                                    what = "Sorting blocks did not round trip through a file";
                                }
                            }
                        }
                        std::remove(path.c_str());
                    }
                }
                if (!ok) {
                    result = fail("Sorting blocks: " + what);
                } else {
                    std::printf("  sorting  : Sticky Note and Group (layered order, "
                                "member list round trip)\n");
                }
            }

            // --- export the real thing ------------------------------------
            // --- shader blocks: derived ports and legacy migration ---------
            if (result == 0) {
                bool ok = true;
                std::string what;
                Graph shaderGraph;
                Node *shaderNode = shaderGraph.addNode("render.shader", 0, 0);
                if (!shaderNode) {
                    ok = false;
                    what = "the Shader block is not registered";
                } else {
                    const std::vector<PortDesc> &defaults = shaderNode->inputPorts();
                    if (defaults.size() != 1 || defaults[0].type != PortType::Image) {
                        ok = false;
                        what = "a Shader block without a file should only show uPrev";
                    }
                    shaderNode->setText("shader", "assets/shaders/spiral_tunnel.glsl");
                    std::string shaderError;
                    if (ok &&
                        !Registry::applyShaderPorts(*shaderNode, renderer.shaders(), &shaderError)) {
                        ok = false;
                        what = "apply shader ports: " + shaderError;
                    }
                    if (ok) {
                        std::vector<std::string> names;
                        for (const PortDesc &port : shaderNode->inputPorts()) {
                            names.push_back(port.name);
                        }
                        auto has = [&](const char *name) {
                            return std::find(names.begin(), names.end(), name) != names.end();
                        };
                        // spiral_tunnel.glsl uses uUser[0], uUser[1] and both colours.
                        if (!has("uPrev") || !has("uUser[0]") || !has("uUser[1]") ||
                            !has("uColorA") || !has("uColorB")) {
                            ok = false;
                            what = "the ports do not follow the uniforms of the shader";
                        }
                    }
                    // The template shaders in assets/shaders are documented
                    // starting points; they have to compile and derive the
                    // ports they advertise.
                    if (ok) {
                        struct ShaderTemplate {
                            const char *file;
                            const char *port;
                        };
                        const ShaderTemplate templates[] = {
                            {"assets/shaders/passthrough.glsl", "uPrev"},
                            {"assets/shaders/scale.glsl", "uUser[0]"},
                            {"assets/shaders/rotation.glsl", "uUser[0]"},
                        };
                        for (const ShaderTemplate &entry : templates) {
                            Graph graph;
                            Node *node = graph.addNode("render.shader", 0, 0);
                            if (!node) {
                                ok = false;
                                what = "the Shader block is not registered";
                                break;
                            }
                            node->setText("shader", entry.file);
                            std::string error;
                            if (!Registry::applyShaderPorts(*node, renderer.shaders(), &error)) {
                                ok = false;
                                what = std::string("template ") + entry.file + ": " + error;
                                break;
                            }
                            bool hasPort = false;
                            for (const PortDesc &port : node->inputPorts()) {
                                if (port.name == entry.port) hasPort = true;
                            }
                            if (!hasPort) {
                                ok = false;
                                what = std::string("template ") + entry.file +
                                       " does not expose " + entry.port;
                                break;
                            }
                        }
                    }
                }

                // A retired "shader.pass" node load: a file makes it a Shader
                // block, a built-in preset makes it Spectrum.
                if (ok) {
                    const std::string legacyPath = "selftest_legacy.pforge";
                    std::ofstream legacyFile(legacyPath.c_str(), std::ios::binary);
                    legacyFile << R"({
  "application": "PulseForge",
  "version": 1,
  "name": "Legacy",
  "video": { "width": 320, "height": 180, "fps": 30 },
  "output": { "container": "mp4" },
  "media": {},
  "blocks": [
    { "id": 1, "kind": "shader.pass", "title": "Shader Pass", "x": 0, "y": 0, "enabled": true,
      "params": { "shader": "assets/shaders/spiral_tunnel.glsl", "preset": 3 } },
    { "id": 2, "kind": "shader.pass", "title": "Shader Pass", "x": 200, "y": 0, "enabled": true,
      "params": { "shader": "", "preset": 4 } }
  ],
  "connections": []
})";
                    legacyFile.close();
                    Project legacy;
                    std::string legacyError;
                    if (!legacy.load(legacyPath, &legacyError, &renderer.shaders())) {
                        ok = false;
                        what = "legacy project failed to load: " + legacyError;
                    } else {
                        const Node *withFile = legacy.graph.find(1);
                        const Node *withPreset = legacy.graph.find(2);
                        const bool fileOk =
                            withFile && withFile->kind == "render.shader" &&
                            withFile->inputPorts().size() > 1;  // ports came from the file
                        const bool presetOk = withPreset && withPreset->kind == "render.spectrum";
                        if (!fileOk || !presetOk) {
                            ok = false;
                            what = "shader.pass was not migrated to Shader/Spectrum";
                        }
                    }
                }

                if (ok) {
                    std::printf("  shader   : derived ports (uPrev/uUser/uColour), "
                                "passthrough/scale/rotation templates and shader.pass "
                                "migration ok\n");
                } else {
                    result = fail("shader block: " + what);
                }

                // A project saved before the Audio Output existed gets one
                // wired to the last Audio-emitting block of its old chain.
                if (result == 0) {
                    const std::string legacyAudioPath = "selftest_legacy_audio.pforge";
                    std::ofstream legacyAudio(legacyAudioPath.c_str(), std::ios::binary);
                    legacyAudio << R"({
  "application": "PulseForge",
  "version": 1,
  "name": "Legacy Audio",
  "video": { "width": 320, "height": 180, "fps": 30 },
  "output": { "container": "mp4" },
  "media": {},
  "blocks": [
    { "id": 1, "kind": "src.audio", "title": "Audio Source", "x": 0, "y": 0, "enabled": true,
      "params": {} },
    { "id": 2, "kind": "dsp.dynamics", "title": "Dynamics", "x": 200, "y": 0, "enabled": true,
      "params": {} },
    { "id": 3, "kind": "out.video", "title": "Video Output", "x": 400, "y": 0, "enabled": true,
      "params": {} }
  ],
  "connections": [
    { "from": 1, "fromPort": 0, "to": 2, "toPort": 0 }
  ]
})";
                    legacyAudio.close();
                    Project legacy;
                    std::string legacyError;
                    if (!legacy.load(legacyAudioPath, &legacyError)) {
                        result = fail("legacy audio project failed to load: " + legacyError);
                    } else {
                        const int audioOut = legacy.graph.audioSinkNodeId();
                        const Link *link = audioOut ? legacy.graph.findInputLink(audioOut, 0) : nullptr;
                        const Node *source = link ? legacy.graph.find(link->fromNode) : nullptr;
                        if (!audioOut || !source || source->kind != "dsp.dynamics") {
                            result = fail("legacy audio project did not gain an Audio Output "
                                          "at the end of its Dynamics chain");
                        } else {
                            std::printf("  legacy   : Audio Output added after Dynamics\n");
                        }
                    }
                }

                // Geometry's old spectrum/waveform shapes migrate to the
                // Spectrum presets that draw the same element (with their
                // parameters), and the remaining primitive indices are
                // remapped.
                if (result == 0) {
                    const std::string legacyGeometryPath = "selftest_legacy_geometry.pforge";
                    std::ofstream legacyGeometry(legacyGeometryPath.c_str(), std::ios::binary);
                    legacyGeometry << R"({
  "application": "PulseForge",
  "version": 1,
  "name": "Legacy Geometry",
  "video": { "width": 320, "height": 180, "fps": 30 },
  "output": { "container": "mp4" },
  "media": {},
  "blocks": [
    { "id": 1, "kind": "dsp.analyze", "title": "Spectrum Analyzer", "x": 0, "y": 0,
      "enabled": true, "params": {} },
    { "id": 2, "kind": "geom.primitives", "title": "Geometry", "x": 200, "y": 0,
      "enabled": true, "params": { "shape": 3, "count": 48, "radius": 0.75, "thickness": 9.5,
                                   "spin": 0.25, "colorA": "#FF0000FF" } },
    { "id": 3, "kind": "geom.primitives", "title": "Geometry", "x": 400, "y": 0,
      "enabled": true, "params": { "shape": 7 } },
    { "id": 5, "kind": "geom.primitives", "title": "Geometry", "x": 400, "y": 200,
      "enabled": true, "params": { "shape": 6, "radius": 0.5, "thickness": 3.5 } },
    { "id": 6, "kind": "geom.primitives", "title": "Geometry", "x": 400, "y": 400,
      "enabled": true, "params": { "shape": 4 } },
    { "id": 7, "kind": "geom.primitives", "title": "Geometry", "x": 400, "y": 600,
      "enabled": true, "params": { "shape": 5 } },
    { "id": 4, "kind": "out.video", "title": "Video Output", "x": 600, "y": 0,
      "enabled": true, "params": {} }
  ],
  "connections": [
    { "from": 2, "fromPort": 0, "to": 4, "toPort": 0 }
  ]
})";
                    legacyGeometry.close();
                    Project legacy;
                    std::string legacyError;
                    if (!legacy.load(legacyGeometryPath, &legacyError,
                                     &renderer.shaders())) {
                        result = fail("legacy geometry project failed to load: " + legacyError);
                    } else {
                        const std::vector<std::string> presets = Registry::spectrumPresetNames();
                        const auto presetIndex = [&](const char *name) {
                            for (size_t i = 0; i < presets.size(); ++i) {
                                if (presets[i] == name) return static_cast<int>(i);
                            }
                            return -1;
                        };
                        const Node *radial = legacy.graph.find(2);
                        const Node *primitive = legacy.graph.find(3);
                        const Node *line = legacy.graph.find(5);
                        const Node *bars = legacy.graph.find(6);
                        const Node *ring = legacy.graph.find(7);
                        const bool spectrumOk = radial && radial->kind == "render.spectrum" &&
                                                radial->pint("preset", -1) ==
                                                    presetIndex("radial_bars") &&
                                                radial->pint("count", -1) == 48 &&
                                                std::fabs(radial->pfloat("radius", 0.0f) -
                                                          0.75f) < 1e-4f &&
                                                std::fabs(radial->pfloat("thickness", 0.0f) -
                                                          9.5f) < 1e-4f &&
                                                std::fabs(radial->pfloat("spin", 0.0f) -
                                                          0.25f) < 1e-4f &&
                                                radial->pcolor("colorA").r == 255 &&
                                                legacy.graph.findInputLink(2, 0) != nullptr;
                        const bool primitiveOk =
                            primitive && primitive->kind == "geom.primitives" &&
                            primitive->pint("shape", -1) == 3;
                        const bool lineOk = line && line->kind == "render.spectrum" &&
                                            line->pint("preset", -1) ==
                                                presetIndex("waveform_line") &&
                                            std::fabs(line->pfloat("radius", 0.0f) - 0.5f) <
                                                1e-4f;
                        const bool shapesOk =
                            bars && bars->kind == "render.spectrum" &&
                            bars->pint("preset", -1) == presetIndex("bar_spectrum") && ring &&
                            ring->kind == "render.spectrum" &&
                            ring->pint("preset", -1) == presetIndex("waveform_ring");
                        if (!spectrumOk || !primitiveOk || !lineOk || !shapesOk) {
                            result =
                                fail("legacy geometry was not migrated to Spectrum/primitives");
                        } else {
                            std::printf("  legacy   : Geometry elements -> Spectrum presets "
                                        "(parameters kept), primitives remapped\n");
                        }
                    }
                }

                // Every Spectrum preset exposes its own ports and Inspector
                // parameters; switching preset rebuilds both and carries input
                // links over by name (dropping the ones that no longer exist).
                if (result == 0) {
                    Graph graph;
                    Node *analyzer = graph.addNode("dsp.analyze", 0.0f, 0.0f);
                    Node *spectrum = graph.addNode("render.spectrum", 200.0f, 0.0f);
                    Node *mod = graph.addNode("math.constant", 200.0f, 140.0f);
                    const std::vector<std::string> presets = Registry::spectrumPresetNames();
                    const auto presetIndex = [&](const char *name) {
                        for (size_t i = 0; i < presets.size(); ++i) {
                            if (presets[i] == name) return static_cast<int>(i);
                        }
                        return -1;
                    };
                    const auto hasInput = [](const Node &node, const char *name) {
                        for (const PortDesc &port : node.inputPorts()) {
                            if (port.name == name) return true;
                        }
                        return false;
                    };
                    std::string what;
                    // Saved projects store the preset *index*, so the list is
                    // append-only: adding a helper effect (such as Transform)
                    // must not shift the shader presets or the four Geometry
                    // elements.
                    bool ok = presets.size() == 19 && presets[4] == "radial_spectrum" &&
                              presets[15] == "radial_bars" &&
                              presets[18] == "waveform_line";
                    if (!ok) what = "the Spectrum preset list changed order";
                    ok = ok && analyzer && spectrum && mod && presetIndex("plasma") >= 0 &&
                         presetIndex("radial_bars") >= 0;
                    if (!ok) {
                        if (what.empty()) what = "Spectrum preset list is missing an effect";
                    } else {
                        std::string why;
                        ok = graph.connect(analyzer->id, 0, spectrum->id, 0, &why);  // Analysis
                        if (ok) ok = graph.connect(mod->id, 0, spectrum->id, 2, &why);  // Feedback
                        if (!ok) what = why;
                    }
                    if (ok) {
                        // The default (passthrough) schema: shader ports and a
                        // Feedback parameter, no Geometry ports.
                        ok = hasInput(*spectrum, "Feedback") && !hasInput(*spectrum, "Position") &&
                             spectrum->find("feedback") != nullptr;
                        if (!ok) what = "the default preset does not expose its shader schema";
                    }
                    if (ok) {
                        spectrum->setInt("preset", presetIndex("plasma"));
                        EvalContext probe;
                        probe.fps = 60.0f;
                        graph.evaluate(probe);
                        ok = hasInput(*spectrum, "Feedback") && hasInput(*spectrum, "speed") &&
                             hasInput(*spectrum, "complexity") &&
                             spectrum->find("speed") != nullptr &&
                             spectrum->find("complexity") != nullptr &&
                             graph.findInputLink(spectrum->id, 0) != nullptr &&
                             graph.findInputLink(spectrum->id, 2) != nullptr;
                        if (!ok) what = "a shader preset does not expose its own ports/params";
                    }
                    if (ok) {
                        spectrum->setFloat("speed", 2.5f);
                        spectrum->setInt("preset", presetIndex("gradient"));
                        EvalContext probe;
                        probe.fps = 60.0f;
                        graph.evaluate(probe);
                        ok = hasInput(*spectrum, "speed") && !hasInput(*spectrum, "complexity") &&
                             std::fabs(spectrum->pfloat("speed", 0.0f) - 2.5f) < 1e-4f;
                        if (!ok) what = "switching preset lost a shared parameter";
                    }
                    if (ok) {
                        // A Geometry preset has no Feedback port, so that link
                        // is dropped while the Analysis link stays.
                        spectrum->setInt("preset", presetIndex("radial_bars"));
                        EvalContext probe;
                        probe.fps = 60.0f;
                        graph.evaluate(probe);
                        ok = hasInput(*spectrum, "Position") && !hasInput(*spectrum, "Feedback") &&
                             spectrum->find("count") != nullptr &&
                             spectrum->find("radius") != nullptr &&
                             spectrum->find("feedback") == nullptr &&
                             graph.findInputLink(spectrum->id, 0) != nullptr &&
                             graph.findInputLink(spectrum->id, 2) == nullptr;
                        if (!ok) what = "a Geometry preset did not rebuild its ports/params";
                    }
                    if (!ok) {
                        result = fail("Spectrum preset schema: " + what);
                    } else {
                        std::printf("  spectrum : preset schemas rebuild ports and parameters\n");
                    }
                }

                // The migrated Geometry elements actually draw into their Image
                // instead of only allocating a black target.
                if (result == 0) {
                    Graph graph;
                    Node *src = graph.addNode("src.audio", 0.0f, 0.0f);
                    Node *analyzer = graph.addNode("dsp.analyze", 200.0f, 0.0f);
                    Node *spectrum = graph.addNode("render.spectrum", 400.0f, 0.0f);
                    Node *output = graph.addNode("out.video", 700.0f, 0.0f);
                    std::string why;
                    bool ok = src && analyzer && spectrum && output &&
                              graph.connect(src->id, 0, analyzer->id, 0, &why) &&
                              graph.connect(analyzer->id, 0, spectrum->id, 0, &why) &&
                              graph.connect(spectrum->id, 0, output->id, 0, &why);
                    if (!ok) {
                        result = fail("Geometry preset render: " + why);
                    } else {
                        const std::vector<std::string> presets =
                            Registry::spectrumPresetNames();
                        for (size_t i = 0; i < presets.size(); ++i) {
                            if (presets[i] == "radial_bars") {
                                spectrum->setInt("preset", static_cast<int>(i));
                            }
                        }
                        // Resolve the migrated preset's schema before its
                        // parameters are edited (the graph does this itself on
                        // the next evaluation).
                        EvalContext shape;
                        shape.fps = 30.0f;
                        shape.width = 320;
                        shape.height = 180;
                        graph.evaluate(shape);
                        spectrum->setFloat("radius", 0.6f);
                        spectrum->setFloat("thickness", 8.0f);
                        EvalContext ctx;
                        ctx.width = 320;
                        ctx.height = 180;
                        ctx.fps = 30.0f;
                        ctx.duration = 1.0;
                        ctx.time = 0.5;
                        ctx.frame = 15;
                        ctx.audioTime = 0.5;
                        ctx.audio = clip.buffer();
                        ctx.analysis = analysis;
                        std::string renderError;
                        if (!renderer.renderFrame(graph, ctx, &renderError)) {
                            result = fail("Geometry preset render: " + renderError);
                        } else {
                            const Node *node = graph.find(spectrum->id);
                            ImageBufferPtr image =
                                node && !node->outputs.empty() ? node->outputs[0].image : nullptr;
                            long long brightness = 0;
                            if (image && image->valid()) {
                                Image pixels = LoadImageFromTexture(image->texture.texture);
                                if (pixels.data) {
                                    const unsigned char *data =
                                        static_cast<const unsigned char *>(pixels.data);
                                    const size_t count = static_cast<size_t>(pixels.width) *
                                                         static_cast<size_t>(pixels.height);
                                    for (size_t i = 0; i < count; ++i) {
                                        brightness += data[i * 4] + data[i * 4 + 1] +
                                                      data[i * 4 + 2];
                                    }
                                }
                                UnloadImage(pixels);
                            }
                            if (brightness <= 0) {
                                result = fail("Geometry preset rendered a black Image");
                            } else {
                                std::printf("  spectrum : Geometry element draws "
                                            "(brightness %lld)\n",
                                            brightness);
                            }
                        }
                    }
                }

                // Transform maps the Image through the Matrix: a 2x zoom grows
                // the disc, and a 3rd-column translation moves it sideways,
                // which pins down the row/column layout the block documents.
                if (result == 0) {
                    Graph transformGraph;
                    const int circleId =
                        transformGraph.addNode("geom.primitives", 0.0f, 0.0f)->id;
                    const int matrixId =
                        transformGraph.addNode("math.matrix", 0.0f, 140.0f)->id;
                    const int transformId =
                        transformGraph.addNode("fx.transform", 260.0f, 0.0f)->id;
                    const int outputId =
                        transformGraph.addNode("out.video", 540.0f, 0.0f)->id;
                    Node *circle = transformGraph.find(circleId);
                    Node *matrix = transformGraph.find(matrixId);
                    Node *transform = transformGraph.find(transformId);
                    Node *output = transformGraph.find(outputId);
                    std::string what;
                    bool ok = circle && matrix && transform && output;
                    if (!ok) {
                        what = "the Transform block is not registered";
                    } else {
                        circle->setInt("shape", 1);  // Circle
                        circle->setFloat("radius", 0.3f);
                        circle->setColor("colorA", WHITE);
                        circle->setColor("colorB", WHITE);
                        matrix->setInt("size", 0);  // 2x2
                        std::string why;
                        ok = transformGraph.connect(circle->id, 0, transform->id, 0, &why) &&
                             transformGraph.connect(matrix->id, 0, transform->id, 1, &why) &&
                             transformGraph.connect(transform->id, 0, output->id, 0, &why);
                        if (!ok) what = why;
                    }
                    const auto renderTransform = [&]() {
                        EvalContext ctx;
                        ctx.width = 320;
                        ctx.height = 180;
                        ctx.fps = 30.0f;
                        ctx.duration = 1.0;
                        ctx.time = 0.5;
                        ctx.frame = 15;
                        ctx.audioTime = 0.5;
                        ctx.audio = clip.buffer();
                        ctx.analysis = analysis;
                        std::string renderError;
                        return renderer.renderFrame(transformGraph, ctx, &renderError) != nullptr;
                    };
                    const auto brightnessAt = [&](float u, float v) {
                        const Node *node = transformGraph.find(transform->id);
                        ImageBufferPtr image =
                            node && !node->outputs.empty() ? node->outputs[0].image : nullptr;
                        if (!image || !image->valid()) return -1;
                        Image pixels = LoadImageFromTexture(image->texture.texture);
                        const int x = std::clamp(static_cast<int>(u * pixels.width), 0,
                                                 pixels.width - 1);
                        const int y = std::clamp(static_cast<int>(v * pixels.height), 0,
                                                 pixels.height - 1);
                        const unsigned char *pixel =
                            static_cast<const unsigned char *>(pixels.data) +
                            (static_cast<size_t>(y) * pixels.width + x) * 4;
                        const int value = pixel[0] + pixel[1] + pixel[2];
                        UnloadImage(pixels);
                        return value;
                    };
                    const auto setMatrix = [&](float scale, float translateX) {
                        if (Param *grid = matrix->find("matrix")) {
                            grid->values[0] = scale;
                            grid->values[5] = scale;
                            grid->values[2] = translateX;  // 3rd column: translation
                        }
                    };
                    if (ok) {
                        setMatrix(1.0f, 0.0f);  // identity
                        ok = renderTransform();
                        if (!ok) what = "the identity frame did not render";
                    }
                    int centre = -1;
                    int leftIdentity = -1;
                    int rightIdentity = -1;
                    int rightZoomed = -1;
                    int leftShifted = -1;
                    int rightShifted = -1;
                    int leftOffset = -1;
                    int rightOffset = -1;
                    if (ok) {
                        centre = brightnessAt(0.5f, 0.5f);
                        leftIdentity = brightnessAt(0.45f, 0.5f);
                        rightIdentity = brightnessAt(0.72f, 0.5f);
                        setMatrix(2.0f, 0.0f);  // 2x zoom
                        ok = renderTransform();
                        if (!ok) what = "the zoomed frame did not render";
                    }
                    if (ok) {
                        rightZoomed = brightnessAt(0.72f, 0.5f);
                        setMatrix(1.0f, 0.25f);  // identity + 3rd-column translation
                        ok = renderTransform();
                        if (!ok) what = "the shifted frame did not render";
                    }
                    if (ok) {
                        leftShifted = brightnessAt(0.45f, 0.5f);
                        rightShifted = brightnessAt(0.72f, 0.5f);
                        // The pre-offset moves the image in the same direction
                        // as the matrix translation.
                        setMatrix(1.0f, 0.0f);
                        transform->setFloat("offsetX", 0.25f);
                        ok = renderTransform();
                        if (!ok) what = "the pre-offset frame did not render";
                    }
                    if (ok) {
                        leftOffset = brightnessAt(0.45f, 0.5f);
                        rightOffset = brightnessAt(0.72f, 0.5f);
                        if (centre < 200) {
                            ok = false;
                            what = "the identity transform lost the image";
                        } else if (leftIdentity < 150 || rightIdentity > 60) {
                            ok = false;
                            what = "the identity transform did not pass the image through";
                        } else if (rightZoomed < 150) {
                            ok = false;
                            what = "a 2x matrix did not zoom the image";
                        } else if (leftShifted > 60 || rightShifted < 150) {
                            ok = false;
                            what = "the matrix translation did not move the image";
                        } else if (leftOffset > 60 || rightOffset < 150) {
                            ok = false;
                            what = "the pre-offset did not move the image";
                        }
                    }
                    if (!ok) {
                        result = fail("Transform block: " + what);
                    } else {
                        std::printf("  render   : Transform passes, zooms and translates "
                                    "(centre %d, 2x %d, shift %d/%d, offset %d/%d)\n",
                                    centre, rightZoomed, leftShifted, rightShifted, leftOffset,
                                    rightOffset);
                    }
                }

                // Picture and Textbox: the two media blocks in Render. The BMP
                // has a red top half and a blue bottom half, so the sampled rows
                // prove the decode *and* the orientation; the Textbox draws a
                // white glyph over it; the gif check proves an animated file
                // advances with the timeline.
                if (result == 0) {
                    const std::string picturePath = "selftest_picture.bmp";
                    bool ok = writePictureBmp(picturePath, 24, 24);
                    std::string what;
                    Graph mediaGraph;
                    int pictureId = 0;
                    int textboxId = 0;
                    if (ok) {
                        pictureId = mediaGraph.addNode("render.picture", 0.0f, 0.0f)->id;
                        textboxId = mediaGraph.addNode("render.textbox", 260.0f, 0.0f)->id;
                        const int outputId = mediaGraph.addNode("out.video", 540.0f, 0.0f)->id;
                        (void)outputId;
                    }
                    Node *picture = mediaGraph.find(pictureId);
                    Node *textbox = mediaGraph.find(textboxId);
                    if (!ok) {
                        what = "could not write the test picture";
                    } else if (!picture || !textbox) {
                        ok = false;
                        what = "the Picture/Textbox blocks are not registered";
                    } else {
                        picture->setText("file", picturePath);
                        picture->setInt("mode", 0);  // Raw: the file's own pixels
                        textbox->setText("text", "T");
                        textbox->setFloat("size", 0.35f);
                        textbox->setFloat("x", 0.2f);
                        textbox->setFloat("y", 0.1f);
                        textbox->setFloat("w", 0.6f);
                        textbox->setFloat("h", 0.8f);
                        textbox->setFloat("backgroundOpacity", 0.0f);
                        std::string why;
                        ok = mediaGraph.connect(picture->id, 0, textbox->id, 0, &why) &&
                             mediaGraph.connect(textbox->id, 0, mediaGraph.sinkNodeId(), 0, &why);
                        if (!ok) what = why;
                    }
                    const auto renderMedia = [&](double time) {
                        EvalContext ctx;
                        ctx.width = 160;
                        ctx.height = 90;
                        ctx.fps = 30.0f;
                        ctx.duration = 1.0;
                        ctx.time = time;
                        ctx.frame = static_cast<int>(time * 30.0);
                        ctx.audioTime = time;
                        std::string renderError;
                        return renderer.renderFrame(mediaGraph, ctx, &renderError) != nullptr;
                    };
                    // Reads one pixel of a node's Image as (r, g, b).
                    const auto pixelAt = [&](const ImageBufferPtr &image, float u, float v) {
                        if (!image || !image->valid()) return Vector3{-1.0f, -1.0f, -1.0f};
                        Image pixels = LoadImageFromTexture(image->texture.texture);
                        const int x = std::clamp(static_cast<int>(u * pixels.width), 0,
                                                 pixels.width - 1);
                        const int y = std::clamp(static_cast<int>(v * pixels.height), 0,
                                                 pixels.height - 1);
                        const unsigned char *pixel =
                            static_cast<const unsigned char *>(pixels.data) +
                            (static_cast<size_t>(y) * pixels.width + x) * 4;
                        const Vector3 value{static_cast<float>(pixel[0]),
                                            static_cast<float>(pixel[1]),
                                            static_cast<float>(pixel[2])};
                        UnloadImage(pixels);
                        return value;
                    };
                    const auto outputImage = [&](int nodeId) {
                        const Node *node = mediaGraph.find(nodeId);
                        return node && !node->outputs.empty() ? node->outputs[0].image
                                                              : ImageBufferPtr{};
                    };
                    if (ok) {
                        ok = renderMedia(0.1);
                        if (!ok) what = "the media frame did not render";
                    }
                    if (ok) {
                        const Vector3 upper = pixelAt(outputImage(pictureId), 0.5f, 0.45f);
                        const Vector3 lower = pixelAt(outputImage(pictureId), 0.5f, 0.55f);
                        const Vector3 corner = pixelAt(outputImage(pictureId), 0.02f, 0.02f);
                        if (upper.x < 180.0f || upper.z > 60.0f || lower.z < 180.0f ||
                            lower.x > 60.0f || corner.x > 20.0f) {
                            ok = false;
                            what = "the picture is missing, flipped or not centred";
                        }
                    }
                    if (ok) {
                        // The glyph is white, the layer under it stays visible.
                        bool foundGlyph = false;
                        ImageBufferPtr image = outputImage(textboxId);
                        if (image && image->valid()) {
                            Image pixels = LoadImageFromTexture(image->texture.texture);
                            for (int y = 0; y < pixels.height && !foundGlyph; ++y) {
                                for (int x = 0; x < pixels.width; ++x) {
                                    const unsigned char *pixel =
                                        static_cast<const unsigned char *>(pixels.data) +
                                        (static_cast<size_t>(y) * pixels.width + x) * 4;
                                    if (pixel[0] > 200 && pixel[1] > 200 && pixel[2] > 200) {
                                        foundGlyph = true;
                                        break;
                                    }
                                }
                            }
                            UnloadImage(pixels);
                        }
                        const Vector3 layer = pixelAt(image, 0.5f, 0.45f);
                        if (!foundGlyph) {
                            ok = false;
                            what = "the Textbox did not draw its text";
                        } else if (layer.x < 180.0f) {
                            ok = false;
                            what = "the Textbox did not keep its Layer";
                        }
                    }
                    if (ok) {
                        // Animated pictures: a two frame gif built with ffmpeg has
                        // to show different frames at different times.
                        const std::string gifPath = "selftest_picture.gif";
                        std::vector<std::string> args = {
                            ffmpeg::ffmpegPath(), "-hide_banner", "-loglevel", "error", "-y",
                            "-f", "lavfi", "-i", "color=c=red:s=16x16:r=10:d=0.1",
                            "-f", "lavfi", "-i", "color=c=blue:s=16x16:r=10:d=0.1",
                            "-filter_complex", "[0:v][1:v]concat=n=2:v=1", gifPath};
                        ChildProcess process;
                        std::string gifError;
                        const bool built = process.start(args, &gifError) &&
                                           process.wait(nullptr) &&
                                           std::ifstream(gifPath.c_str()).good();
                        if (built) {
                            Graph gifGraph;
                            const int gifPicture = gifGraph.addNode("render.picture", 0, 0)->id;
                            const int gifOutput = gifGraph.addNode("out.video", 400, 0)->id;
                            gifGraph.find(gifPicture)->setText("file", gifPath);
                            std::string why;
                            ok = gifGraph.connect(gifPicture, 0, gifOutput, 0, &why);
                            const auto gifColour = [&](double time) {
                                EvalContext ctx;
                                ctx.width = 160;
                                ctx.height = 90;
                                ctx.fps = 30.0f;
                                ctx.duration = 1.0;
                                ctx.time = time;
                                ctx.frame = static_cast<int>(time * 30.0);
                                ctx.audioTime = time;
                                std::string renderError;
                                if (!renderer.renderFrame(gifGraph, ctx, &renderError)) {
                                    return Vector3{-1.0f, -1.0f, -1.0f};
                                }
                                const Node *node = gifGraph.find(gifPicture);
                                return pixelAt(node && !node->outputs.empty()
                                                   ? node->outputs[0].image
                                                   : ImageBufferPtr{},
                                               0.5f, 0.5f);
                            };
                            // Walk the first loop and require both frames to
                            // show up; the exported rate is whatever the file
                            // carries, so no single time pair is guaranteed.
                            bool sawRed = false;
                            bool sawBlue = false;
                            for (int step = 0; step < 21 && ok; ++step) {
                                const Vector3 colour = gifColour(0.005 + 0.01 * step);
                                if (colour.x > 150.0f && colour.z < 60.0f) sawRed = true;
                                if (colour.z > 150.0f && colour.x < 60.0f) sawBlue = true;
                            }
                            if (!sawRed || !sawBlue) {
                                ok = false;
                                what = "the animated picture did not advance";
                            }
                        }
                        std::remove(gifPath.c_str());
                    }
                    if (ok) {
                        // The blocks also have to survive a real export, not only
                        // the preview: mux a few frames through ffmpeg.
                        Project mediaProject;
                        mediaProject.graph = mediaGraph;
                        mediaProject.name = "Media blocks";
                        mediaProject.video.width = 320;
                        mediaProject.video.height = 180;
                        mediaProject.video.fps = 30.0;
                        mediaProject.video.useAudioDuration = false;
                        mediaProject.video.duration = 0.3;
                        mediaProject.output = outputSpecForContainer("mp4");
                        mediaProject.output.videoCodec = "libx264";
                        ExportRequest request;
                        request.outputPath = "selftest_media.mp4";
                        request.overwrite = true;
                        request.audioSampleRate = 48000;
                        std::string exportError;
                        double lastProgressTime = -1.0;
                        bool progressAdvanced = true;
                        ok = Exporter::run(renderer, mediaProject, request, AudioPtr{}, nullptr,
                                           [&](const ExportProgress &progress) {
                                               if (progress.videoTime < lastProgressTime) {
                                                   progressAdvanced = false;
                                               }
                                               lastProgressTime = progress.videoTime;
                                           },
                                           nullptr, &exportError);
                        if (!ok) {
                            what = "media export: " + exportError;
                        } else if (!progressAdvanced ||
                                   lastProgressTime < mediaProject.video.duration - 1e-3) {
                            ok = false;
                            what = "the export progress did not advance to the end";
                        } else if (!ffmpeg::probe("selftest_media.mp4").ok) {
                            ok = false;
                            what = "the exported media video is unreadable";
                        }
                    }
                    std::remove(picturePath.c_str());
                    if (!ok) {
                        result = fail("Picture/Textbox blocks: " + what);
                    } else {
                        std::printf("  render   : Picture decodes upright, Textbox draws over its "
                                    "layer, animated gif advances\n");
                    }
                }

                // Self Reference: the editor window as an Image. Paint a known
                // pattern into the framebuffer, capture it, and check the block
                // hands it out the right way up (and that it is lazy - no
                // capture, no image).
                if (result == 0) {
                    Graph selfGraph;
                    Node *selfRef = selfGraph.addNode("render.selfref", 0.0f, 0.0f);
                    Node *selfOut = selfGraph.addNode("out.video", 400.0f, 0.0f);
                    bool ok = selfRef && selfOut;
                    std::string what;
                    if (!ok) {
                        what = "the Self Reference block is not registered";
                    } else if (!selfRef->inputPorts().empty() ||
                               selfRef->outputPorts().size() != 1 ||
                               selfRef->outputPorts()[0].type != PortType::Image) {
                        ok = false;
                        what = "Self Reference ports are wrong";
                    } else {
                        std::string why;
                        ok = selfGraph.connect(selfRef->id, 0, selfOut->id, 0, &why);
                        if (!ok) what = why;
                    }
                    if (ok) {
                        // Without a capture the block has nothing to hand out.
                        EvalContext empty;
                        empty.renderer = &renderer;
                        empty.width = 160;
                        empty.height = 90;
                        selfGraph.evaluate(empty);
                        if (selfRef->outputs[0].image) {
                            ok = false;
                            what = "Self Reference produced an Image before any capture";
                        }
                    }
                    if (ok) {
                        // Top half red, bottom half blue, then capture: the image
                        // has to keep the screen's top row at its top.
                        BeginDrawing();
                        ClearBackground(BLACK);
                        DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight() / 2, RED);
                        DrawRectangle(0, GetScreenHeight() / 2, GetScreenWidth(),
                                      GetScreenHeight() - GetScreenHeight() / 2, BLUE);
                        renderer.refreshScreenCapture();
                        EndDrawing();
                        const ImageBufferPtr &capture = renderer.screenCapture();
                        if (!capture || !capture->valid()) {
                            ok = false;
                            what = "the capture texture was not created";
                        } else {
                            Image pixels = LoadImageFromTexture(capture->texture.texture);
                            const auto at = [&](float v) {
                                const int y = std::clamp(static_cast<int>(v * pixels.height), 0,
                                                         pixels.height - 1);
                                const int x = pixels.width / 2;
                                const unsigned char *pixel =
                                    static_cast<const unsigned char *>(pixels.data) +
                                    (static_cast<size_t>(y) * pixels.width + x) * 4;
                                return Vector3{static_cast<float>(pixel[0]),
                                               static_cast<float>(pixel[1]),
                                               static_cast<float>(pixel[2])};
                            };
                            const Vector3 top = at(0.2f);
                            const Vector3 bottom = at(0.8f);
                            UnloadImage(pixels);
                            if (top.x < 180.0f || top.z > 60.0f || bottom.z < 180.0f ||
                                bottom.x > 60.0f) {
                                ok = false;
                                what = "the capture is flipped or empty";
                            }
                        }
                    }
                    if (ok) {
                        EvalContext ctx;
                        ctx.renderer = &renderer;
                        ctx.width = 160;
                        ctx.height = 90;
                        selfGraph.evaluate(ctx);
                        const ImageBufferPtr &image = selfRef->outputs[0].image;
                        if (!image || !image->valid()) {
                            ok = false;
                            what = "the Self Reference did not hand out the capture";
                        } else if (image->width != renderer.screenCapture()->width) {
                            ok = false;
                            what = "the Self Reference handed out a copy, not the capture";
                        }
                    }
                    if (!ok) {
                        result = fail("Self Reference block: " + what);
                    } else {
                        std::printf("  render   : Self Reference captures the window upright and "
                                    "only on demand\n");
                    }
                }

                // A processed Analysis input only carries one video frame of
                // audio, so the waveform presets keep a rolling history: the
                // wave has to fill the bar (sampling past the buffer used to
                // clamp to the first/last sample and draw a flat, moving line
                // beside the real wave) and the preview has to reach the same
                // span the export shows.
                if (result == 0) {
                    Graph waveGraph;
                    const int srcId = waveGraph.addNode("src.audio", 0.0f, 0.0f)->id;
                    const int analyzerId = waveGraph.addNode("dsp.analyze", 200.0f, 0.0f)->id;
                    const int spectrumId = waveGraph.addNode("render.spectrum", 400.0f, 0.0f)->id;
                    const int outputId = waveGraph.addNode("out.video", 700.0f, 0.0f)->id;
                    Node *src = waveGraph.find(srcId);
                    Node *analyzer = waveGraph.find(analyzerId);
                    Node *spectrum = waveGraph.find(spectrumId);
                    Node *output = waveGraph.find(outputId);
                    const std::vector<std::string> presets = Registry::spectrumPresetNames();
                    const auto presetIndex = [&](const char *name) {
                        for (size_t i = 0; i < presets.size(); ++i) {
                            if (presets[i] == name) return static_cast<int>(i);
                        }
                        return -1;
                    };
                    std::string what;
                    bool ok = src && analyzer && spectrum && output;
                    if (ok) {
                        spectrum->setInt("preset", presetIndex("waveform_line"));
                        EvalContext shape;
                        shape.fps = 60.0f;
                        shape.width = 320;
                        shape.height = 180;
                        waveGraph.evaluate(shape);  // resolve the preset schema
                        spectrum->setFloat("thickness", 1.0f);
                        std::string why;
                        ok = waveGraph.connect(src->id, 0, analyzer->id, 0, &why) &&
                             waveGraph.connect(analyzer->id, 0, spectrum->id, 0, &why) &&
                             waveGraph.connect(spectrum->id, 0, output->id, 0, &why);
                        if (!ok) what = why;
                    }
                    const auto makeWindow = [](long long startFrame, float frequency) {
                        auto buffer = std::make_shared<AudioBuffer>();
                        buffer->channels = 1;
                        buffer->sampleRate = 48000;
                        buffer->startFrame = startFrame;
                        buffer->frameCount = 960;  // 20 ms: shorter than the 80 ms view
                        buffer->samples.resize(960);
                        for (int i = 0; i < 960; ++i) {
                            buffer->samples[static_cast<size_t>(i)] =
                                0.8f * std::sin(6.2831853f * frequency *
                                                static_cast<float>(startFrame + i) / 48000.0f);
                        }
                        return buffer;
                    };
                    auto firstWindow = makeWindow(2000, 200.0f);    // slow half
                    auto secondWindow = makeWindow(2960, 800.0f);   // fast half
                    auto thirdWindow = makeWindow(3920, 800.0f);
                    const auto renderFrame = [&](const std::shared_ptr<AudioBuffer> &audio,
                                                 int frame) {
                        EvalContext ctx;
                        ctx.width = 320;
                        ctx.height = 180;
                        ctx.fps = 60.0f;
                        ctx.duration = 1.0;
                        ctx.time = static_cast<double>(frame) / 60.0;
                        ctx.frame = frame;
                        ctx.audioTime = ctx.time;
                        ctx.audio = audio;
                        std::string renderError;
                        return renderer.renderFrame(waveGraph, ctx, &renderError);
                    };
                    const auto brightestRow = [](const Image &pixels, int x) {
                        int best = -1;
                        int bestValue = 0;
                        for (int y = 0; y < pixels.height; ++y) {
                            const unsigned char *pixel =
                                static_cast<const unsigned char *>(pixels.data) +
                                (static_cast<size_t>(y) * pixels.width + x) * 4;
                            const int value = pixel[0] + pixel[1] + pixel[2];
                            if (value > bestValue) {
                                bestValue = value;
                                best = y;
                            }
                        }
                        return bestValue > 0 ? best : -1;
                    };
                    // Counts how often the drawn line crosses the centre, which
                    // is the signal's zero crossing: a slow wave on the left of
                    // the bar means the history reached further back than the
                    // incoming window.
                    const auto centreCrossings = [&](const Image &pixels, int firstX, int lastX) {
                        int crossings = 0;
                        int previous = 0;
                        for (int x = firstX; x < lastX; ++x) {
                            const int row = brightestRow(pixels, x);
                            if (row < 0) continue;
                            const int sign = row < pixels.height / 2 ? 1 : -1;
                            if (previous != 0 && sign != previous) ++crossings;
                            previous = sign;
                        }
                        return crossings;
                    };
                    const auto imageOf = [&]() {
                        const Node *node = waveGraph.find(spectrum->id);
                        return node && !node->outputs.empty() ? node->outputs[0].image
                                                              : ImageBufferPtr{};
                    };
                    if (ok) {
                        if (!renderFrame(firstWindow, 3)) {  // no whole-file analysis: live
                            ok = false;
                            what = "the first frame did not render";
                        }
                    }
                    if (ok) {
                        ImageBufferPtr image = imageOf();
                        if (!image || !image->valid()) {
                            ok = false;
                            what = "no waveform image";
                        } else {
                            Image pixels = LoadImageFromTexture(image->texture.texture);
                            const int left = brightestRow(pixels, 0);
                            const int right = brightestRow(pixels, pixels.width - 1);
                            const int nearLeft = brightestRow(pixels, 2);
                            if (left < 0 || right < 0) {
                                ok = false;
                                what = "the waveform does not span the bar";
                            } else if (left == nearLeft) {
                                ok = false;
                                what = "the waveform starts on a flat clamped line";
                            }
                            UnloadImage(pixels);
                        }
                    }
                    if (ok && !renderFrame(secondWindow, 4)) {
                        ok = false;
                        what = "the second frame did not render";
                    }
                    if (ok && !renderFrame(thirdWindow, 5)) {
                        ok = false;
                        what = "the third frame did not render";
                    }
                    if (ok) {
                        const Node *node = waveGraph.find(spectrum->id);
                        if (!node || node->waveCount != 2880 ||
                            node->waveEndFrame != 4880) {
                            ok = false;
                            what = "the wave history did not accumulate the windows";
                        }
                    }
                    if (ok) {
                        ImageBufferPtr image = imageOf();
                        if (!image || !image->valid()) {
                            ok = false;
                            what = "no waveform image";
                        } else {
                            Image pixels = LoadImageFromTexture(image->texture.texture);
                            const int leftCrossings =
                                centreCrossings(pixels, 0, pixels.width / 4);
                            const int rightCrossings =
                                centreCrossings(pixels, pixels.width * 3 / 4, pixels.width);
                            // Left quarter: one 20 ms window of the slow wave.
                            // Right quarter: the fast wave. Without the history
                            // both quarters would show the fast wave.
                            if (leftCrossings * 2 > rightCrossings) {
                                ok = false;
                                what = "the preview wave did not reach the export span";
                            } else {
                                std::printf("  spectrum : waveform fills the bar and keeps "
                                            "history (crossings %d vs %d)\n",
                                            leftCrossings, rightCrossings);
                            }
                            UnloadImage(pixels);
                        }
                    }
                    if (!ok) result = fail("Spectrum waveform window: " + what);
                }

                // The export file name has to follow the output container.
                if (result == 0) {
                    const std::string renamed =
                        exportPathForContainer("C:/out/output.mp4", "webm", "C:/out");
                    const std::string appended =
                        exportPathForContainer("C:/out/my clip", "avi", "C:/out");
                    const std::string fresh = exportPathForContainer("", "mkv", "C:/out");
                    if (renamed != "C:/out/output.webm" || appended != "C:/out/my clip.avi" ||
                        fresh != "C:/out/output.mkv") {
                        result = fail("export path does not follow the container");
                    } else {
                        std::printf("  export   : file name follows the container\n");
                    }
                }

                // GPU encoders: the table has to suit the container and the
                // ffmpeg arguments have to use the per-family quality options.
                if (result == 0) {
                    bool ok = true;
                    std::string what;
                    auto has = [](const std::vector<std::string> &list, const std::string &id) {
                        return std::find(list.begin(), list.end(), id) != list.end();
                    };
                    const std::vector<std::string> webm = videoEncodersForContainer("webm");
                    if (has(webm, "libx264") || !has(webm, "libvpx-vp9")) {
                        ok = false;
                        what = "the WebM encoder list offers codecs the container cannot take";
                    }
                    if (!videoEncoderIsHardware("h264_nvenc") ||
                        videoEncoderIsHardware("libx264")) {
                        ok = false;
                        what = "the hardware flag is wrong";
                    }
                    auto commandFor = [&](const std::string &codec) {
                        Project probe = project;
                        probe.output.container = "mp4";
                        probe.output.videoCodec = codec;
                        ExportRequest request;
                        request.outputPath = "probe.mp4";
                        return Exporter::describeCommand(
                            Exporter::buildCommand(probe, request, 1));
                    };
                    const std::string nvenc = commandFor("h264_nvenc");
                    const std::string x264 = commandFor("libx264");
                    const std::string qsv = commandFor("h264_qsv");
                    const std::string hevc = commandFor("hevc_nvenc");
                    if (nvenc.find("-cq") == std::string::npos ||
                        nvenc.find("-crf") != std::string::npos) {
                        ok = false;
                        what = "NVENC should use -cq, not -crf";
                    }
                    if (x264.find("-crf") == std::string::npos) {
                        ok = false;
                        what = "x264 lost its -crf";
                    }
                    if (qsv.find("-global_quality") == std::string::npos) {
                        ok = false;
                        what = "Quick Sync should use -global_quality";
                    }
                    if (hevc.find("hvc1") == std::string::npos) {
                        ok = false;
                        what = "HEVC in MP4 needs the hvc1 tag";
                    }
                    if (ok) {
                        std::printf("  encoders : per-family options ok, NVENC runs: %s\n",
                                    ffmpeg::canRunVideoEncoder("h264_nvenc") ? "yes" : "no");
                    } else {
                        result = fail("video encoders: " + what);
                    }
                }

                // Audio codec matching: a PCM file must not become raw PCM in a
                // Matroska file (players decode that to silence), while a
                // container that cannot take FLAC keeps PCM.
                if (result == 0) {
                    const bool allAvailable = true;
                    auto anyEncoder = [&](const std::string &) { return allAvailable; };
                    const AudioEncoderChoice mkvPcm =
                        chooseAudioEncoder("pcm_f32le", 6144, "mkv", anyEncoder);
                    const AudioEncoderChoice aviPcm =
                        chooseAudioEncoder("pcm_s16le", 1536, "avi", anyEncoder);
                    const AudioEncoderChoice webmOpus =
                        chooseAudioEncoder("opus", 141, "webm", anyEncoder);
                    const AudioEncoderChoice opusToMp4 =
                        chooseAudioEncoder("opus", 141, "mp4", anyEncoder);
                    const bool pcmOk = mkvPcm.encoder == "flac" && !mkvPcm.transcodeToAac;
                    const bool aviOk = aviPcm.encoder == "pcm_s16le";
                    const bool webmOk = webmOpus.encoder == "libopus";
                    // Opus cannot go into MP4 here, so it becomes AAC in memory.
                    const bool mp4Ok = opusToMp4.transcodeToAac && opusToMp4.encoder == "aac";
                    if (!pcmOk || !aviOk || !webmOk || !mp4Ok) {
                        result = fail("audio encoder matching: mkv pcm=" + mkvPcm.encoder +
                                      " avi pcm=" + aviPcm.encoder + " webm opus=" +
                                      webmOpus.encoder + " mp4 opus=" + opusToMp4.encoder);
                    } else {
                        std::printf("  audio    : PCM->FLAC in Matroska, container rules ok\n");
                    }
                }

                // Debug and modulation blocks: passthrough metering, non-finite
                // guarding, the ring buffer and the modulation biquad.
                if (result == 0) {
                    bool ok = true;
                    std::string what;
                    EvalContext blockCtx;
                    blockCtx.fps = 60.0f;
                    blockCtx.duration = 4.0;
                    blockCtx.width = 320;
                    blockCtx.height = 180;

                    // VU meter: the value must reach the output untouched, and
                    // 0.125 (-18 dBFS) has to read as 0 VU.
                    Graph meterGraph;
                    Node *meterSource = meterGraph.addNode("math.constant", 0, 0);
                    Node *meter = meterGraph.addNode("dbg.meter", 200, 0);
                    if (!meterSource || !meter) {
                        ok = false;
                        what = "meter blocks are not registered";
                    } else {
                        // Amplitude of -18 dBFS: this has to read as 0 VU.
                        const float minus18 = std::pow(10.0f, -18.0f / 20.0f);
                        meterSource->setFloat("value", minus18);
                        meterGraph.connect(meterSource->id, 0, meter->id, 0);
                        meterGraph.evaluate(blockCtx);
                        if (std::fabs(meter->outputs[0].scalar - minus18) > 1e-6f) {
                            ok = false;
                            what = "the VU meter changed its input";
                        } else if (std::fabs(meter->runtimeState["vu"]) > 0.01) {
                            ok = false;
                            what = "-18 dBFS should read as 0 VU";
                        }
                    }

                    // VU ballistics: the needle averages the rectified signal and
                    // falls slowly, while the digital meter follows the raw value
                    // with no release at all.
                    if (ok) {
                        Graph ballistics;
                        Node *vu = ballistics.addNode("dbg.meter", 0, 0);
                        Node *vuSource = ballistics.addNode("math.constant", 200, 0);
                        Node *digital = ballistics.addNode("dbg.meter", 400, 0);
                        Node *digitalSource = ballistics.addNode("math.constant", 600, 0);
                        if (!vu || !vuSource || !digital || !digitalSource) {
                            ok = false;
                            what = "meter blocks are not registered";
                        } else {
                            digital->setInt("mode", 1);
                            std::string why;
                            ok = ballistics.connect(vuSource->id, 0, vu->id, 0, &why) &&
                                 ballistics.connect(digitalSource->id, 0, digital->id, 0, &why);
                            if (!ok) what = why;
                        }
                        const auto lastPlotted = [](const Node &node) {
                            const int capacity = static_cast<int>(node.historyA.size());
                            if (capacity <= 0) return 0.0f;
                            return Node::historyAt(node.historyA, capacity, node.historyCount, 1.0f);
                        };
                        if (ok) {
                            EvalContext ctx;
                            ctx.fps = 60.0f;
                            ctx.duration = 1.0;
                            // 0.1 s of full scale, then silence.
                            for (int frame = 0; frame < 6; ++frame) {
                                vuSource->setFloat("value", 1.0f);
                                digitalSource->setFloat("value", 1.0f);
                                ctx.frame = frame;
                                ctx.time = frame / 60.0;
                                ballistics.evaluate(ctx);
                            }
                            const double vuRising = vu->runtimeState["average"];
                            const float digitalFull = lastPlotted(*digital);
                            for (int frame = 6; frame < 8; ++frame) {
                                vuSource->setFloat("value", 0.0f);
                                digitalSource->setFloat("value", 0.0f);
                                ctx.frame = frame;
                                ctx.time = frame / 60.0;
                                ballistics.evaluate(ctx);
                            }
                            const double vuFalling = vu->runtimeState["average"];
                            const float digitalSilent = lastPlotted(*digital);
                            // Averaging: 0.1 s of signal does not reach full scale.
                            // Slow release: two silent frames barely move it.
                            // Digital: exactly the raw value, both times.
                            if (vuRising < 0.3 || vuRising > 0.9) {
                                ok = false;
                                what = "the VU average is not averaged (" +
                                       std::to_string(vuRising) + ")";
                            } else if (vuFalling < 0.4) {
                                ok = false;
                                what = "the VU release is too fast (" +
                                       std::to_string(vuFalling) + ")";
                            } else if (std::fabs(digitalFull - 1.0f) > 1e-4f ||
                                       std::fabs(digitalSilent) > 1e-4f ||
                                       std::fabs(static_cast<float>(digital->runtimeState["average"])) >
                                           1e-4f) {
                                ok = false;
                                what = "the Digital meter is not showing raw values (full " +
                                       std::to_string(digitalFull) + ", silent " +
                                       std::to_string(digitalSilent) + ", avg " +
                                       std::to_string(digital->runtimeState["average"]) + ")";
                            }
                        }
                    }

                    // Guard: NaN and both infinities are silenced and flagged.
                    Graph guardGraph;
                    Node *guardSource = guardGraph.addNode("math.constant", 0, 0);
                    Node *guard = guardGraph.addNode("dbg.guard", 200, 0);
                    if (!guardSource || !guard) {
                        ok = false;
                        what = "the Guard block is not registered";
                    } else {
                        guardGraph.connect(guardSource->id, 0, guard->id, 0);
                        const float probes[] = {std::numeric_limits<float>::quiet_NaN(),
                                                std::numeric_limits<float>::infinity(),
                                                -std::numeric_limits<float>::infinity(), 0.25f};
                        const char *lamps[] = {"nan", "pos", "neg", nullptr};
                        for (int i = 0; i < 4 && ok; ++i) {
                            guardSource->setFloat("value", probes[i]);
                            guardGraph.evaluate(blockCtx);
                            if (lamps[i] && guard->runtimeState[lamps[i]] < 0.9) {
                                ok = false;
                                what = std::string("the ") + lamps[i] + " lamp did not light";
                            }
                            const float expected = i == 3 ? 0.25f : 0.0f;
                            if (std::fabs(guard->outputs[0].scalar - expected) > 1e-6f) {
                                ok = false;
                                what = "the Guard did not silence a non-finite value";
                            }
                        }
                    }

                    // Ring buffer: live value, running average and the looping tap.
                    Graph ringGraph;
                    Node *ringSource = ringGraph.addNode("math.constant", 0, 0);
                    Node *ring = ringGraph.addNode("mod.ringbuffer", 200, 0);
                    float ringAverage = 0.0f;
                    if (!ringSource || !ring) {
                        ok = false;
                        what = "the Ringbuffer block is not registered";
                    } else {
                        ring->setInt("size", 16);
                        ring->setFloat("speed", 1.0f);
                        ringSource->setFloat("value", 0.5f);
                        ringGraph.connect(ringSource->id, 0, ring->id, 0);
                        for (int i = 0; i < 40; ++i) ringGraph.evaluate(blockCtx);
                        const float live = ring->outputs[0].scalar;
                        const float average = ring->outputs[1].scalar;
                        const float buffered = ring->outputs[2].scalar;
                        ringAverage = average;
                        if (std::fabs(live - 0.5f) > 1e-6f ||
                            std::fabs(average - 0.5f) > 1e-4f) {
                            ok = false;
                            what = "ring buffer input/average";
                        } else if (buffered < -0.001f || buffered > 0.501f) {
                            ok = false;
                            what = "ring buffer tap left the buffer";
                        }
                    }

                    // Signal Filter: DC passes the low pass, is removed by the
                    // high pass and by the band pass.
                    auto filterStep = [&](int mode, float *result) {
                        Graph filterGraph;
                        Node *filterSource = filterGraph.addNode("math.constant", 0, 0);
                        Node *filterNode = filterGraph.addNode("mod.filter", 200, 0);
                        if (!filterSource || !filterNode) return false;
                        filterSource->setFloat("value", 1.0f);
                        filterNode->setInt("mode", mode);
                        filterNode->setFloat("cutoff", 10.0f);
                        filterNode->setFloat("resonance", 0.707f);
                        filterGraph.connect(filterSource->id, 0, filterNode->id, 0);
                        for (int i = 0; i < 240; ++i) filterGraph.evaluate(blockCtx);
                        *result = filterNode->outputs[0].scalar;
                        return true;
                    };
                    float lowPass = 0.0f, highPass = 0.0f, bandPass = 0.0f;
                    if (!filterStep(0, &lowPass) || !filterStep(1, &highPass) ||
                        !filterStep(2, &bandPass)) {
                        ok = false;
                        what = "the Signal Filter block is not registered";
                    } else if (lowPass < 0.95f || std::fabs(highPass) > 0.05f ||
                               std::fabs(bandPass) > 0.05f) {
                        ok = false;
                        what = "biquad response is wrong (lp=" + std::to_string(lowPass) +
                               " hp=" + std::to_string(highPass) +
                               " bp=" + std::to_string(bandPass) + ")";
                    }

                    // The published response has to match the coefficients: the
                    // low pass is flat well below cutoff and -3 dB at cutoff.
                    const BiquadCoefficients low = biquadCoefficients(0, 10.0, 0.707, 60.0);
                    const double passband = biquadMagnitudeDb(low, 0.5, 60.0);
                    const double atCutoff = biquadMagnitudeDb(low, 10.0, 60.0);
                    const double stopband = biquadMagnitudeDb(low, 29.0, 60.0);
                    if (std::fabs(passband) > 0.5 || std::fabs(atCutoff + 3.0) > 0.7 ||
                        stopband > -12.0) {
                        ok = false;
                        what = "biquad magnitude response";
                    }

                    if (ok) {
                        std::printf("  blocks   : meter %.3f, guard lamps, ringbuffer average "
                                    "%.3f, biquad lp=%.3f hp=%.3f bp=%.3f, VU averages and "
                                    "releases slowly while the digital meter stays raw\n",
                                    meterSource ? meterSource->pfloat("value") : 0.0f, ringAverage,
                                    lowPass, highPass, bandPass);
                    } else {
                        result = fail("debug/modulation blocks: " + what);
                    }

                    // Dynamics: static curve, stream processing and graph
                    // wiring, including the appended modulation ports.
                    if (result == 0) {
                        bool dynOk = true;
                        std::string dynWhat;
                        DynamicsSettings statics;
                        statics.thresholdDb = -18.0f;
                        statics.ratio = 4.0f;
                        const float compression = dynamicsGainDb(statics, -6.0f);   // -9 dB
                        DynamicsSettings expansion = statics;
                        expansion.expand = true;
                        expansion.ratio = 2.0f;
                        const float expanded = dynamicsGainDb(expansion, -30.0f);   // -12 dB
                        if (std::fabs(compression + 9.0f) > 0.05f ||
                            std::fabs(expanded + 12.0f) > 0.05f) {
                            dynOk = false;
                            dynWhat = "static curve (comp " + std::to_string(compression) +
                                      ", exp " + std::to_string(expanded) + ")";
                        }

                        // A settled compressor at -6 dBFS with a 4:1 ratio over a
                        // -18 dBFS trigger has to land 9 dB down.
                        DynamicsSettings settings;
                        settings.thresholdDb = -18.0f;
                        settings.ratio = 4.0f;
                        settings.attackMs = 10.0f;
                        settings.releaseMs = 150.0f;
                        settings.limiter = false;
                        DynamicsState state;
                        std::vector<float> buffer(48000, 0.5f);
                        std::vector<float> processed(buffer.size(), 0.0f);
                        processDynamicsBlock(buffer.data(), processed.data(),
                                             static_cast<long long>(buffer.size()), 1, 48000,
                                             settings, state);
                        const float settled = processed.back();
                        const float expected = 0.5f * std::pow(10.0f, -9.0f / 20.0f);
                        if (std::fabs(settled - expected) > 0.01f) {
                            dynOk = false;
                            dynWhat = "stream gain (" + std::to_string(settled) + " vs " +
                                      std::to_string(expected) + ")";
                        }

                        // The limiter has to hold a 2.0 peak at or below 0 dBFS;
                        // hard clip is transparent below the ceiling.
                        settings.thresholdDb = 0.0f;   // keep the compressor out of the way
                        settings.ratio = 1.0f;
                        settings.limiter = true;
                        settings.softClip = false;
                        settings.limiterAttackMs = 0.1f;
                        DynamicsState limiterState;
                        std::vector<float> hot(4800, 2.0f);
                        std::vector<float> limited(hot.size(), 0.0f);
                        processDynamicsBlock(hot.data(), limited.data(),
                                             static_cast<long long>(hot.size()), 1, 48000, settings,
                                             limiterState);
                        if (limited.back() > 1.0001f || limited.back() < 0.99f) {
                            dynOk = false;
                            dynWhat = "hard clip limiter (" + std::to_string(limited.back()) + ")";
                        }

                        // Graph wiring: the Audio output carries the processed
                        // clip and a Threshold modulation moves the published
                        // value the inspector follows.
                        Graph dynamicsGraph;
                        Node *dynamics = dynamicsGraph.addNode("dsp.dynamics", 0, 0);
                        Node *thresholdSource = dynamicsGraph.addNode("math.constant", 220, 0);
                        float dryDb = -120.0f;
                        float wetDb = -120.0f;
                        if (!dynamics || !thresholdSource) {
                            dynOk = false;
                            dynWhat = "the Dynamics block is not registered";
                        } else {
                            if (dynamics->inputPorts().size() != 7 ||
                                dynamics->outputPorts().size() != 1 ||
                                dynamics->inputPorts()[0].type != PortType::Audio ||
                                dynamics->outputPorts()[0].type != PortType::Audio) {
                                dynOk = false;
                                dynWhat = "Dynamics ports are wrong";
                            }
                            thresholdSource->setFloat("value", -0.5f);  // -12 dB
                            dynamicsGraph.connect(thresholdSource->id, 0, dynamics->id, 2);
                            auto clipBuffer = std::make_shared<AudioBuffer>();
                            clipBuffer->channels = 1;
                            clipBuffer->sampleRate = 48000;
                            clipBuffer->frameCount = 48000;
                            clipBuffer->samples.assign(48000, 0.5f);
                            EvalContext ctx;
                            ctx.fps = 60.0f;
                            ctx.duration = 1.0;
                            ctx.audio = clipBuffer;
                            ctx.audioTime = 0.0;
                            for (int frame = 0; frame < 12; ++frame) {
                                ctx.frame = frame;
                                // Preview invariant: time follows the playhead
                                // and audioTime follows the clip, so the block
                                // snaps its window to the video frame grid.
                                ctx.time = static_cast<double>(frame) / 60.0;
                                ctx.audioTime = static_cast<double>(frame) / 60.0;
                                dynamicsGraph.evaluate(ctx);
                            }
                            float published = 0.0f;
                            const bool hasPublished = dynamics->effectiveParam("threshold", &published);
                            dryDb = static_cast<float>(
                                dynamics->runtimeState.count("dryDb")
                                    ? dynamics->runtimeState.at("dryDb")
                                    : -120.0);
                            wetDb = static_cast<float>(
                                dynamics->runtimeState.count("wetDb")
                                    ? dynamics->runtimeState.at("wetDb")
                                    : -120.0);
                            if (!dynamics->outputs[0].audio ||
                                dynamics->outputs[0].type != PortType::Audio) {
                                dynOk = false;
                                dynWhat = "Dynamics did not emit an Audio buffer";
                            } else if (!hasPublished || std::fabs(published + 30.0f) > 0.1f) {
                                dynOk = false;
                                dynWhat = "Threshold modulation did not publish";
                            } else if (!(wetDb < dryDb - 4.0f)) {
                                dynOk = false;
                                dynWhat = "Dynamics did not attenuate the clip (dry " +
                                          std::to_string(dryDb) + ", wet " + std::to_string(wetDb) +
                                          ")";
                            }
                        }

                        const NodeDef *noise = Registry::instance().find("dsp.noise");
                        if (!noise || noise->category != "Math") {
                            dynOk = false;
                            dynWhat = "Noise is not in the Math category";
                        }

                        if (dynOk) {
                            std::printf("  dynamics : curve %.2f/%.2f dB, settled %.4f, limiter "
                                        "%.3f, graph dry %.1f wet %.1f dBFS\n",
                                        compression, expanded, settled, limited.back(), dryDb, wetDb);
                        } else {
                            result = fail("dynamics block: " + dynWhat);
                        }
                    }

                    // Audio bridges: a DAC turns a Scalar into samples and an
                    // ADC measures the stream back at the video frame rate.
                    if (result == 0) {
                        Graph bridge;
                        Node *constant = bridge.addNode("math.constant", 0, 0);
                        Node *dac = bridge.addNode("dsp.dac", 200, 0);
                        Node *adc = bridge.addNode("dsp.adc", 400, 0);
                        bool bridgeOk = constant && dac && adc;
                        std::string bridgeWhat;
                        if (!bridgeOk) {
                            bridgeWhat = "the ADC/DAC blocks are not registered";
                        } else {
                            constant->setFloat("value", 0.25f);
                            dac->setBool("interpolate", true);
                            adc->setInt("mode", 1);  // RMS; mode 0 is Unity
                            std::string why;
                            bridgeOk = bridge.connect(constant->id, 0, dac->id, 0, &why) &&
                                       bridge.connect(dac->id, 0, adc->id, 0, &why);
                            if (!bridgeOk) bridgeWhat = why;
                        }
                        auto source = std::make_shared<AudioBuffer>();
                        source->channels = 1;
                        source->sampleRate = 48000;
                        source->frameCount = 96000;
                        source->samples.assign(96000, 0.5f);
                        EvalContext bridgeCtx;
                        bridgeCtx.fps = 60.0f;
                        bridgeCtx.duration = 1.0;
                        bridgeCtx.offline = true;
                        bridgeCtx.audio = source;
                        float adcValue = 0.0f;
                        if (bridgeOk) {
                            // Start at 1 s so the rendered buffer carries a
                            // non-zero startFrame, the case a downstream ADC has
                            // to map onto the clip.
                            for (int step = 0; step < 60; ++step) {
                                bridgeCtx.frame = 60 + step;
                                bridgeCtx.audioTime = 1.0 + static_cast<double>(step) / 60.0;
                                bridge.evaluate(bridgeCtx);
                            }
                            adcValue = adc->outputs[0].asScalar();
                            const AudioBuffer *rendered = dac->audioRenderOutput.get();
                            if (!rendered || rendered->frameCount != 48000 ||
                                rendered->startFrame != 48000) {
                                bridgeOk = false;
                                bridgeWhat = "the DAC did not render the 1 s window";
                            } else if (std::fabs(rendered->samples[24000] - 0.25f) > 0.01f) {
                                bridgeOk = false;
                                bridgeWhat = "the DAC sample does not match the Scalar";
                            } else if (std::fabs(adcValue - 0.25f) > 0.01f) {
                                bridgeOk = false;
                                bridgeWhat = "the ADC did not measure the DAC stream";
                            }
                        }
                        if (bridgeOk) {
                            std::printf("  bridges  : DAC rendered %.3f, ADC measured %.3f\n",
                                        static_cast<double>(dac->audioRenderOutput->samples[24000]),
                                        static_cast<double>(adcValue));
                        } else {
                            result = fail("ADC/DAC blocks: " + bridgeWhat);
                        }
                    }

                    // ADC Unity -> DAC is a lossless round trip, and a Math
                    // block in between shapes the carried waveform.
                    if (result == 0) {
                        auto waveform = std::make_shared<AudioBuffer>();
                        waveform->channels = 2;
                        waveform->sampleRate = 48000;
                        waveform->frameCount = 96000;
                        waveform->samples.resize(static_cast<size_t>(waveform->frameCount) * 2);
                        for (long long i = 0; i < waveform->frameCount; ++i) {
                            const float sample =
                                0.4f * std::sin(6.2831853f * 440.0f * i / 48000.0f);
                            waveform->samples[static_cast<size_t>(i) * 2] = sample;
                            waveform->samples[static_cast<size_t>(i) * 2 + 1] = sample;
                        }

                        const auto renderBridge = [&](bool shaped, AudioPtr *output,
                                                      std::string *why) {
                            Graph graph;
                            Node *src = graph.addNode("src.audio", 0, 0);
                            Node *adc = graph.addNode("dsp.adc", 200, 0);
                            Node *math = shaped ? graph.addNode("math.arithmetic", 400, 0) : nullptr;
                            Node *dac = graph.addNode("dsp.dac", 600, 0);
                            if (!src || !adc || !dac || (shaped && !math)) {
                                *why = "blocks missing";
                                return false;
                            }
                            std::string connectWhy;
                            bool ok = graph.connect(src->id, 0, adc->id, 0, &connectWhy) &&
                                      graph.connect(adc->id, 0, shaped ? math->id : dac->id, 0,
                                                    &connectWhy);
                            if (ok && shaped) {
                                math->setInt("op", 2);  // multiply
                                math->setFloat("bValue", 0.5f);
                                ok = graph.connect(math->id, 0, dac->id, 0, &connectWhy);
                            }
                            if (!ok) {
                                *why = connectWhy;
                                return false;
                            }
                            EvalContext ctx;
                            ctx.fps = 60.0f;
                            ctx.duration = 2.0;
                            ctx.offline = true;
                            ctx.audio = waveform;
                            for (int step = 0; step < 60; ++step) {
                                ctx.frame = 60 + step;
                                ctx.audioTime = 1.0 + static_cast<double>(step) / 60.0;
                                graph.evaluate(ctx);
                            }
                            *output = dac->audioRenderOutput;
                            return true;
                        };

                        AudioPtr straight;
                        AudioPtr shaped;
                        std::string straightWhy;
                        std::string shapedWhy;
                        bool rtOk = renderBridge(false, &straight, &straightWhy) &&
                                    renderBridge(true, &shaped, &shapedWhy);
                        if (rtOk && (!straight || straight->frameCount != 48000 ||
                                     straight->startFrame != 48000 || straight->channels != 2 ||
                                     straight->sampleRate != 48000)) {
                            rtOk = false;
                            straightWhy = "wrong rendered format";
                        }
                        double rtDiff = 0.0;
                        if (rtOk) {
                            const size_t base = 48000u * 2u;
                            bool finite = true;
                            for (size_t i = 0; i < straight->samples.size(); ++i) {
                                const double diff =
                                    std::fabs(static_cast<double>(straight->samples[i]) -
                                              waveform->samples[base + i]);
                                if (!std::isfinite(diff)) {
                                    finite = false;
                                    break;
                                }
                                rtDiff = std::max(rtDiff, diff);
                            }
                            if (!finite) {
                                rtOk = false;
                                straightWhy = "non-finite round trip sample";
                            } else if (rtDiff > 1e-7) {
                                rtOk = false;
                                straightWhy = "round trip max diff " + std::to_string(rtDiff);
                            }
                        }
                        double shapeDiff = 1e9;
                        if (rtOk) {
                            if (!shaped || shaped->samples.size() != straight->samples.size()) {
                                rtOk = false;
                                shapedWhy = "shaped render missing";
                            } else {
                                shapeDiff = 0.0;
                                for (size_t i = 0; i < shaped->samples.size(); ++i) {
                                    shapeDiff = std::max(
                                        shapeDiff,
                                        std::fabs(static_cast<double>(shaped->samples[i]) * 2.0 -
                                                  straight->samples[i]));
                                }
                                if (shapeDiff > 1e-5) {
                                    rtOk = false;
                                    shapedWhy = "Math x0.5 did not shape the carrier";
                                }
                            }
                        }
                        if (rtOk) {
                            std::printf("  bridges  : Unity round trip diff %.1e, Math x0.5 diff "
                                        "%.1e\n",
                                        rtDiff, shapeDiff);
                        } else {
                            result = fail("ADC/DAC round trip: " + straightWhy + shapedWhy);
                        }
                    }

                    // A nonlinear audio-rate chain: x * 2.0838 through tanh has
                    // to match the same formula sample by sample.
                    if (result == 0) {
                        Graph chain;
                        Node *src = chain.addNode("src.audio", 0, 0);
                        Node *adc = chain.addNode("dsp.adc", 200, 0);
                        Node *arith = chain.addNode("math.arithmetic", 400, 0);
                        Node *hyper = chain.addNode("math.hyperbolic", 600, 0);
                        Node *dac = chain.addNode("dsp.dac", 800, 0);
                        bool chainOk = src && adc && arith && hyper && dac;
                        std::string chainWhat;
                        if (!chainOk) {
                            chainWhat = "blocks missing";
                        } else {
                            arith->setInt("op", 2);  // multiply
                            arith->setFloat("bValue", 2.083770752f);
                            std::string why;
                            chainOk = chain.connect(src->id, 0, adc->id, 0, &why) &&
                                      chain.connect(adc->id, 0, arith->id, 0, &why) &&
                                      chain.connect(arith->id, 0, hyper->id, 0, &why) &&
                                      chain.connect(hyper->id, 2, dac->id, 0, &why);
                            if (!chainOk) chainWhat = why;
                        }
                        if (chainOk) {
                            auto tone = std::make_shared<AudioBuffer>();
                            tone->channels = 2;
                            tone->sampleRate = 48000;
                            tone->frameCount = 96000;
                            tone->samples.resize(static_cast<size_t>(tone->frameCount) * 2);
                            for (long long i = 0; i < tone->frameCount; ++i) {
                                const float sample =
                                    0.4f * std::sin(6.2831853f * 440.0f * i / 48000.0f);
                                tone->samples[static_cast<size_t>(i) * 2] = sample;
                                tone->samples[static_cast<size_t>(i) * 2 + 1] = sample;
                            }
                            EvalContext ctx;
                            ctx.fps = 60.0f;
                            ctx.duration = 2.0;
                            ctx.offline = true;
                            ctx.audio = tone;
                            for (int step = 0; step < 60; ++step) {
                                ctx.frame = 60 + step;
                                ctx.audioTime = 1.0 + static_cast<double>(step) / 60.0;
                                chain.evaluate(ctx);
                            }
                            const AudioBuffer *out = dac->audioRenderOutput.get();
                            if (!out || out->frameCount != 48000) {
                                chainOk = false;
                                chainWhat = "wrong rendered window";
                            } else {
                                const size_t base = 48000u * 2u;
                                double maxDiff = 0.0;
                                for (size_t i = 0; i < out->samples.size(); ++i) {
                                    const double expected =
                                        std::tanh(2.083770752 * tone->samples[base + i]);
                                    maxDiff = std::max(
                                        maxDiff,
                                        std::fabs(static_cast<double>(out->samples[i]) - expected));
                                }
                                if (maxDiff > 1e-5) {
                                    chainOk = false;
                                    chainWhat = "tanh chain max diff " + std::to_string(maxDiff);
                                } else {
                                    std::printf("  bridges  : audio-rate tanh chain max diff %.1e\n",
                                                maxDiff);
                                }
                            }
                        }
                        if (!chainOk) result = fail("audio-rate chain: " + chainWhat);
                    }

                    // A Dynamics block grows its buffer every frame; the audio
                    // region must keep appending one continuous track instead of
                    // restarting when that buffer's length changes.
                    if (result == 0) {
                        auto tone = std::make_shared<AudioBuffer>();
                        tone->channels = 2;
                        tone->sampleRate = 48000;
                        tone->frameCount = 48000;
                        tone->samples.resize(static_cast<size_t>(tone->frameCount) * 2);
                        for (long long i = 0; i < tone->frameCount; ++i) {
                            const float sample =
                                0.4f * std::sin(6.2831853f * 440.0f * i / 48000.0f);
                            tone->samples[static_cast<size_t>(i) * 2] = sample;
                            tone->samples[static_cast<size_t>(i) * 2 + 1] = sample;
                        }
                        Graph grown;
                        Node *src = grown.addNode("src.audio", 0, 0);
                        Node *dynamics = grown.addNode("dsp.dynamics", 200, 0);
                        Node *adc = grown.addNode("dsp.adc", 400, 0);
                        Node *dac = grown.addNode("dsp.dac", 600, 0);
                        bool grownOk = src && dynamics && adc && dac;
                        std::string grownWhat;
                        if (!grownOk) {
                            grownWhat = "blocks missing";
                        } else {
                            dynamics->setFloat("threshold", 0.0f);
                            dynamics->setFloat("ratio", 1.0f);
                            dynamics->setBool("limiter", false);
                            std::string why;
                            grownOk = grown.connect(src->id, 0, dynamics->id, 0, &why) &&
                                      grown.connect(dynamics->id, 0, adc->id, 0, &why) &&
                                      grown.connect(adc->id, 0, dac->id, 0, &why);
                            if (!grownOk) grownWhat = why;
                        }
                        if (grownOk) {
                            EvalContext ctx;
                            ctx.fps = 60.0f;
                            ctx.duration = 1.0;
                            ctx.offline = true;
                            ctx.audio = tone;
                            for (int frame = 0; frame < 60; ++frame) {
                                ctx.frame = frame;
                                ctx.audioTime = static_cast<double>(frame) / 60.0;
                                grown.evaluate(ctx);
                            }
                            const AudioBuffer *out = dac->audioRenderOutput.get();
                            if (!out || out->frameCount != 48000) {
                                grownOk = false;
                                grownWhat = "the growing source restarted the DAC buffer";
                            } else {
                                double maxDiff = 0.0;
                                for (size_t i = 0; i < out->samples.size(); ++i) {
                                    maxDiff = std::max(
                                        maxDiff, std::fabs(static_cast<double>(out->samples[i]) -
                                                           tone->samples[i]));
                                }
                                if (maxDiff > 1e-5) {
                                    grownOk = false;
                                    grownWhat = "growing source diff " + std::to_string(maxDiff);
                                } else {
                                    std::printf("  bridges  : growing Dynamics source kept the track "
                                                "(diff %.1e)\n",
                                                maxDiff);
                                }
                            }
                        }
                        if (!grownOk) result = fail("audio-rate growing source: " + grownWhat);
                    }

                    // Display ticks inside one video frame must reuse the same
                    // Dynamics window; restarting it caused periodic holes.
                    if (result == 0) {
                        auto tone = std::make_shared<AudioBuffer>();
                        tone->channels = 2;
                        tone->sampleRate = 48000;
                        tone->frameCount = 96000;
                        tone->samples.resize(static_cast<size_t>(tone->frameCount) * 2);
                        for (long long i = 0; i < tone->frameCount; ++i) {
                            const float sample =
                                0.4f * std::sin(6.2831853f * 440.0f * i / 48000.0f);
                            tone->samples[static_cast<size_t>(i) * 2] = sample;
                            tone->samples[static_cast<size_t>(i) * 2 + 1] = sample;
                        }
                        Graph liveGraph;
                        Node *src = liveGraph.addNode("src.audio", 0, 0);
                        Node *dynamics = liveGraph.addNode("dsp.dynamics", 200, 0);
                        bool liveOk = src && dynamics;
                        std::string liveWhat;
                        if (liveOk) {
                            dynamics->setFloat("threshold", 0.0f);
                            dynamics->setFloat("ratio", 1.0f);
                            dynamics->setBool("limiter", false);
                            std::string why;
                            liveOk = liveGraph.connect(src->id, 0, dynamics->id, 0, &why);
                            if (!liveOk) liveWhat = why;
                        } else {
                            liveWhat = "blocks missing";
                        }
                        if (liveOk) {
                            EvalContext ctx;
                            ctx.fps = 60.0f;
                            ctx.duration = 2.0;
                            ctx.offline = false;
                            ctx.audio = tone;
                            ctx.frame = 10;
                            ctx.time = 10.0 / 60.0;
                            ctx.audioTime = ctx.time;
                            liveGraph.evaluate(ctx);
                            const long long firstStart = dynamics->audioRenderStart;
                            const long long firstFrames = dynamics->audioRenderFrames;
                            for (int tick = 1; tick <= 2; ++tick) {
                                ctx.time = 10.0 / 60.0 + tick / 240.0;
                                ctx.audioTime = ctx.time;
                                liveGraph.evaluate(ctx);
                            }
                            const bool stable = dynamics->audioRenderStart == firstStart &&
                                                dynamics->audioRenderFrames == firstFrames;
                            ctx.frame = 11;
                            ctx.time = 11.0 / 60.0;
                            ctx.audioTime = ctx.time;
                            liveGraph.evaluate(ctx);
                            const bool appended =
                                dynamics->audioRenderStart == firstStart &&
                                dynamics->audioRenderFrames == firstFrames + 800;
                            double maxDiff = 0.0;
                            if (appended && dynamics->audioRenderOutput) {
                                const AudioBuffer &buffer = *dynamics->audioRenderOutput;
                                for (size_t i = 0; i < buffer.samples.size(); ++i) {
                                    maxDiff = std::max(
                                        maxDiff,
                                        std::fabs(static_cast<double>(buffer.samples[i]) -
                                                  tone->samples[static_cast<size_t>(firstStart) *
                                                                   2 +
                                                                i]));
                                }
                            }
                            if (!stable || !appended || maxDiff > 1e-5) {
                                liveOk = false;
                                liveWhat = "sub-frame ticks restarted the Dynamics buffer";
                            } else {
                                std::printf("  dynamics : sub-frame ticks reuse the window (start "
                                            "%lld, %lld frames)\n",
                                            firstStart, dynamics->audioRenderFrames);
                            }
                        }
                        if (!liveOk) result = fail("Dynamics live window: " + liveWhat);
                    }

                    // Pre/post-gain are applied outside the compressor's
                    // ballistics, so a frame-rate modulation step has to be
                    // ramped across the window; a step is audible as cracking.
                    if (result == 0) {
                        auto flat = std::make_shared<AudioBuffer>();
                        flat->channels = 1;
                        flat->sampleRate = 48000;
                        flat->frameCount = 48000;
                        flat->samples.assign(48000, 0.5f);
                        Graph gainGraph;
                        Node *dynamics = gainGraph.addNode("dsp.dynamics", 0, 0);
                        Node *preGain = gainGraph.addNode("math.constant", 200, 0);
                        bool gainOk = dynamics && preGain;
                        std::string gainWhat;
                        if (gainOk) {
                            dynamics->setFloat("threshold", 0.0f);
                            dynamics->setFloat("ratio", 1.0f);
                            dynamics->setBool("limiter", false);
                            preGain->setFloat("value", 0.0f);  // 0 dB
                            std::string why;
                            gainOk = gainGraph.connect(preGain->id, 0, dynamics->id, 1, &why);
                            if (!gainOk) gainWhat = why;
                        } else {
                            gainWhat = "blocks missing";
                        }
                        double maxStep = 0.0;
                        if (gainOk) {
                            EvalContext ctx;
                            ctx.fps = 60.0f;
                            ctx.duration = 1.0;
                            ctx.offline = false;
                            ctx.audio = flat;
                            ctx.frame = 0;
                            ctx.time = 0.0;
                            ctx.audioTime = 0.0;
                            gainGraph.evaluate(ctx);
                            preGain->setFloat("value", 1.0f);  // +6 dB
                            ctx.frame = 1;
                            ctx.time = 1.0 / 60.0;
                            ctx.audioTime = ctx.time;
                            gainGraph.evaluate(ctx);
                            const AudioBuffer *rendered = dynamics->audioRenderOutput.get();
                            if (!rendered || rendered->frameCount < 1600) {
                                gainOk = false;
                                gainWhat = "no rendered window";
                            } else {
                                for (int i = 0; i < 800; ++i) {
                                    const double previous = i == 0
                                                                ? rendered->samples[799]
                                                                : rendered->samples[799 + i];
                                    maxStep = std::max(
                                        maxStep,
                                        std::fabs(static_cast<double>(rendered->samples[800 + i]) -
                                                  previous));
                                }
                                if (std::fabs(rendered->samples[1599] - 1.0f) > 1e-3) {
                                    gainOk = false;
                                    gainWhat = "the ramp did not reach +6 dB (" +
                                               std::to_string(rendered->samples[1599]) + ")";
                                } else if (maxStep > 0.01) {
                                    gainOk = false;
                                    gainWhat = "the pre-gain stepped (max " +
                                               std::to_string(maxStep) + ")";
                                }
                            }
                        }
                        // Reversing the modulation must ramp back down from the
                        // value the previous window ended on.
                        if (gainOk) {
                            preGain->setFloat("value", 0.0f);
                            EvalContext ctx;
                            ctx.fps = 60.0f;
                            ctx.duration = 1.0;
                            ctx.offline = false;
                            ctx.audio = flat;
                            ctx.frame = 2;
                            ctx.time = 2.0 / 60.0;
                            ctx.audioTime = ctx.time;
                            gainGraph.evaluate(ctx);
                            const AudioBuffer *rendered = dynamics->audioRenderOutput.get();
                            if (!rendered || rendered->frameCount < 2400) {
                                gainOk = false;
                                gainWhat = "no third window";
                            } else {
                                double step = 0.0;
                                for (int i = 0; i < 800; ++i) {
                                    const double previous =
                                        i == 0 ? rendered->samples[1599]
                                               : rendered->samples[1599 + i];
                                    step = std::max(
                                        step,
                                        std::fabs(static_cast<double>(rendered->samples[1600 + i]) -
                                                  previous));
                                }
                                if (step > 0.01 || std::fabs(rendered->samples[2399] - 0.5f) > 1e-3) {
                                    gainOk = false;
                                    gainWhat = "the release stepped (max " +
                                               std::to_string(step) + ")";
                                }
                            }
                        }
                        if (!gainOk) {
                            result = fail("Dynamics gain ramp: " + gainWhat);
                        } else {
                            std::printf("  dynamics : modulated pre-gain ramps (max step %.5f)\n",
                                        maxStep);
                        }
                    }

                    // A Dynamics in front of an ADC -> DAC region must not
                    // punch holes into the preview stream: the display runs
                    // faster than the video, so several ticks land on the same
                    // frame and the region has to read the window the Dynamics
                    // produced for that frame.
                    if (result == 0) {
                        auto constant = std::make_shared<AudioBuffer>();
                        constant->channels = 1;
                        constant->sampleRate = 48000;
                        constant->frameCount = 96000;
                        constant->samples.assign(static_cast<size_t>(constant->frameCount), 0.5f);
                        Graph chain;
                        Node *src = chain.addNode("src.audio", 0, 0);
                        Node *dynamics = chain.addNode("dsp.dynamics", 200, 0);
                        Node *adc = chain.addNode("dsp.adc", 400, 0);
                        Node *dac = chain.addNode("dsp.dac", 600, 0);
                        bool chainOk = src && dynamics && adc && dac;
                        std::string chainWhat;
                        if (chainOk) {
                            dynamics->setFloat("threshold", 0.0f);
                            dynamics->setFloat("ratio", 1.0f);
                            dynamics->setBool("limiter", false);
                            std::string why;
                            chainOk = chain.connect(src->id, 0, dynamics->id, 0, &why) &&
                                      chain.connect(dynamics->id, 0, adc->id, 0, &why) &&
                                      chain.connect(adc->id, 0, dac->id, 0, &why) &&
                                      chain.connect(adc->id, 1, dac->id, 1, &why);
                            if (!chainOk) chainWhat = why;
                        } else {
                            chainWhat = "blocks missing";
                        }
                        if (chainOk) {
                            EvalContext ctx;
                            ctx.fps = 60.0f;
                            ctx.duration = 2.0;
                            ctx.offline = false;
                            ctx.audio = constant;
                            for (int frame = 0; frame < 8 && chainOk; ++frame) {
                                const double frameTime = static_cast<double>(frame) / 60.0;
                                // Three display ticks per video frame, the way a
                                // 144+ Hz monitor drives the preview.
                                for (int tick = 0; tick < 3 && chainOk; ++tick) {
                                    ctx.frame = frame;
                                    ctx.time = frameTime + tick / 180.0;
                                    ctx.audioTime = ctx.time;
                                    chain.evaluate(ctx);
                                }
                                const AudioBuffer *window =
                                    dac->audioRenderOutput ? dac->audioRenderOutput.get() : nullptr;
                                if (!window || window->frameCount <= 0) {
                                    chainOk = false;
                                    chainWhat = "the DAC produced no window";
                                    break;
                                }
                                double worst = 0.0;
                                for (const float sample : window->samples) {
                                    worst = std::max(
                                        worst,
                                        std::fabs(static_cast<double>(sample) - 0.5));
                                }
                                if (worst > 1e-3) {
                                    chainOk = false;
                                    chainWhat = "zero-filled preview window (worst " +
                                                std::to_string(worst) + ")";
                                }
                            }
                        }
                        if (!chainOk) {
                            result = fail("Dynamics preview stream: " + chainWhat);
                        } else {
                            std::printf(
                                "  dynamics : preview stays continuous through ADC -> DAC\n");
                        }
                    }

                    // An ADC with no Audio input is silence, never the imported
                    // clip, both inside a region and on its own.
                    if (result == 0) {
                        auto loud = std::make_shared<AudioBuffer>();
                        loud->channels = 2;
                        loud->sampleRate = 48000;
                        loud->frameCount = 48000;
                        loud->samples.assign(96000, 0.5f);
                        Graph silentRegion;
                        Node *adc = silentRegion.addNode("dsp.adc", 0, 0);
                        Node *dac = silentRegion.addNode("dsp.dac", 200, 0);
                        bool silentOk = adc && dac;
                        std::string silentWhat;
                        if (silentOk) {
                            std::string why;
                            silentOk = silentRegion.connect(adc->id, 0, dac->id, 0, &why);
                            if (!silentOk) silentWhat = why;
                        } else {
                            silentWhat = "blocks missing";
                        }
                        if (silentOk) {
                            EvalContext ctx;
                            ctx.fps = 60.0f;
                            ctx.duration = 1.0;
                            ctx.offline = true;
                            ctx.audio = loud;
                            for (int frame = 0; frame < 60; ++frame) {
                                ctx.frame = frame;
                                ctx.audioTime = static_cast<double>(frame) / 60.0;
                                silentRegion.evaluate(ctx);
                            }
                            const AudioBuffer *out = dac->audioRenderOutput.get();
                            float peak = 0.0f;
                            if (out) {
                                for (float sample : out->samples) {
                                    peak = std::max(peak, std::fabs(sample));
                                }
                            }
                            // A disconnected ADC on its own must be silent too.
                            Graph solo;
                            Node *soloAdc = solo.addNode("dsp.adc", 0, 0);
                            if (soloAdc) solo.evaluate(ctx);
                            const float soloValue =
                                soloAdc && !soloAdc->outputs.empty()
                                    ? soloAdc->outputs[0].asScalar()
                                    : 1.0f;
                            if (!out || peak > 1e-6f || std::fabs(soloValue) > 1e-6f) {
                                silentOk = false;
                                silentWhat = "an unconnected ADC produced audio";
                            } else {
                                std::printf("  bridges  : unconnected ADC is silent (region and "
                                            "direct)\n");
                            }
                        }
                        if (!silentOk) result = fail("ADC silence: " + silentWhat);
                    }

                    // Stereo: the left and right samples must stay separate
                    // through a two-port ADC -> DAC region.
                    if (result == 0) {
                        auto stereo = std::make_shared<AudioBuffer>();
                        stereo->channels = 2;
                        stereo->sampleRate = 48000;
                        stereo->frameCount = 48000;
                        stereo->samples.resize(static_cast<size_t>(stereo->frameCount) * 2);
                        for (long long i = 0; i < stereo->frameCount; ++i) {
                            stereo->samples[static_cast<size_t>(i) * 2] =
                                0.4f * std::sin(6.2831853f * 440.0f * i / 48000.0f);
                            stereo->samples[static_cast<size_t>(i) * 2 + 1] =
                                0.25f * std::sin(6.2831853f * 880.0f * i / 48000.0f);
                        }
                        Graph split;
                        Node *src = split.addNode("src.audio", 0, 0);
                        Node *adc = split.addNode("dsp.adc", 200, 0);
                        Node *dac = split.addNode("dsp.dac", 400, 0);
                        bool stereoOk = src && adc && dac;
                        std::string stereoWhat;
                        if (!stereoOk) {
                            stereoWhat = "blocks missing";
                        } else if (adc->outputPorts().size() != 2 ||
                                   dac->inputPorts().size() != 2) {
                            stereoOk = false;
                            stereoWhat = "default channel ports are not stereo";
                        } else {
                            std::string why;
                            stereoOk = split.connect(src->id, 0, adc->id, 0, &why) &&
                                       split.connect(adc->id, 0, dac->id, 0, &why) &&
                                       split.connect(adc->id, 1, dac->id, 1, &why);
                            if (!stereoOk) stereoWhat = why;
                        }
                        if (stereoOk) {
                            EvalContext ctx;
                            ctx.fps = 60.0f;
                            ctx.duration = 1.0;
                            ctx.offline = true;
                            ctx.audio = stereo;
                            for (int frame = 0; frame < 60; ++frame) {
                                ctx.frame = frame;
                                ctx.audioTime = static_cast<double>(frame) / 60.0;
                                split.evaluate(ctx);
                            }
                            const AudioBuffer *out = dac->audioRenderOutput.get();
                            if (!out || out->frameCount != 48000 || out->channels != 2) {
                                stereoOk = false;
                                stereoWhat = "wrong stereo window";
                            } else {
                                double maxDiff = 0.0;
                                for (size_t i = 0; i < out->samples.size(); ++i) {
                                    maxDiff = std::max(
                                        maxDiff,
                                        std::fabs(static_cast<double>(out->samples[i]) -
                                                  stereo->samples[i]));
                                }
                                if (maxDiff > 1e-7) {
                                    stereoOk = false;
                                    stereoWhat = "stereo diff " + std::to_string(maxDiff);
                                } else {
                                    std::printf("  bridges  : stereo left/right kept separate "
                                                "(diff %.1e)\n",
                                                maxDiff);
                                }
                            }
                        }
                        if (!stereoOk) result = fail("stereo ADC/DAC: " + stereoWhat);
                    }

                    // Live preview follows the video frame: a second display tick
                    // inside the same frame reuses the rendered window instead of
                    // advancing the chain again.
                    if (result == 0) {
                        auto tone = std::make_shared<AudioBuffer>();
                        tone->channels = 2;
                        tone->sampleRate = 48000;
                        tone->frameCount = 48000;
                        tone->samples.assign(static_cast<size_t>(tone->frameCount) * 2, 0.1f);
                        Graph live;
                        Node *src = live.addNode("src.audio", 0, 0);
                        Node *adc = live.addNode("dsp.adc", 200, 0);
                        Node *dac = live.addNode("dsp.dac", 400, 0);
                        bool liveOk = src && adc && dac;
                        std::string liveWhat;
                        if (liveOk) {
                            std::string why;
                            liveOk = live.connect(src->id, 0, adc->id, 0, &why) &&
                                     live.connect(adc->id, 0, dac->id, 0, &why) &&
                                     live.connect(adc->id, 1, dac->id, 1, &why);
                            if (!liveOk) liveWhat = why;
                        } else {
                            liveWhat = "blocks missing";
                        }
                        if (liveOk) {
                            EvalContext ctx;
                            ctx.fps = 60.0f;
                            ctx.duration = 1.0;
                            ctx.offline = false;
                            ctx.audio = tone;
                            ctx.frame = 30;
                            ctx.time = 0.5;
                            ctx.audioTime = 0.5;
                            live.evaluate(ctx);
                            const AudioBuffer *first = dac->audioRenderOutput.get();
                            const long long firstStart = first ? first->startFrame : -1;
                            ctx.time += 0.002;
                            ctx.audioTime += 0.002;
                            live.evaluate(ctx);  // still frame 30
                            const bool reused = dac->audioRenderOutput.get() == first;
                            ctx.frame = 31;
                            ctx.time = 31.0 / 60.0;
                            ctx.audioTime = 31.0 / 60.0;
                            live.evaluate(ctx);
                            const AudioBuffer *second = dac->audioRenderOutput.get();
                            const bool advanced =
                                second && second != first && second->startFrame == 31 * 800;
                            if (!first || firstStart != 30 * 800 || !reused || !advanced) {
                                liveOk = false;
                                liveWhat = "frame quantisation or reuse failed";
                            } else {
                                std::printf("  bridges  : live frame reuse ok (start %lld -> %lld)\n",
                                            firstStart, second->startFrame);
                            }
                        }
                        if (!liveOk) result = fail("live audio region: " + liveWhat);
                    }

                    // The Spectrum Analyzer depends only on the Audio wired into
                    // its port: silent when unconnected, the whole-file analysis
                    // for the source clip, and a live window for processed audio.
                    if (result == 0) {
                        auto tone = std::make_shared<AudioBuffer>();
                        tone->channels = 2;
                        tone->sampleRate = 48000;
                        tone->frameCount = 48000;
                        tone->samples.resize(static_cast<size_t>(tone->frameCount) * 2);
                        for (long long i = 0; i < tone->frameCount; ++i) {
                            const float sample =
                                0.4f * std::sin(6.2831853f * 440.0f * i / 48000.0f);
                            tone->samples[static_cast<size_t>(i) * 2] = sample;
                            tone->samples[static_cast<size_t>(i) * 2 + 1] = sample;
                        }
                        AnalysisPtr precomputed = analyzeAudio(*tone, AnalysisSettings{}, {}, tone);
                        Graph analyzerGraph;
                        Node *analyzer = analyzerGraph.addNode("dsp.analyze", 0, 0);
                        Node *src = analyzerGraph.addNode("src.audio", 200, 0);
                        bool analyzerOk = analyzer && src;
                        std::string analyzerWhat;
                        if (!analyzerOk) {
                            analyzerWhat = "the analyzer/source blocks are missing";
                        }
                        EvalContext analyzerCtx;
                        analyzerCtx.fps = 60.0f;
                        analyzerCtx.duration = 1.0;
                        analyzerCtx.offline = true;
                        analyzerCtx.audio = tone;
                        analyzerCtx.analysis = precomputed;
                        analyzerCtx.time = 0.5;
                        analyzerCtx.audioTime = 0.5;
                        analyzerCtx.frame = 30;
                        if (analyzerOk) {
                            analyzerGraph.evaluate(analyzerCtx);
                            if (analyzer->outputs[0].analysis ||
                                std::fabs(analyzer->outputs[1].scalar) > 1e-6f) {
                                analyzerOk = false;
                                analyzerWhat = "unconnected analyzer still produced data";
                            }
                        }
                        if (analyzerOk) {
                            std::string why;
                            if (!analyzerGraph.connect(src->id, 0, analyzer->id, 0, &why)) {
                                analyzerOk = false;
                                analyzerWhat = why;
                            } else {
                                analyzerGraph.evaluate(analyzerCtx);
                                if (analyzer->outputs[0].analysis != precomputed) {
                                    analyzerOk = false;
                                    analyzerWhat = "the source input did not use its whole-file analysis";
                                } else if (analyzer->outputs[1].scalar < 0.1f) {
                                    analyzerOk = false;
                                    analyzerWhat = "the source input measured silence";
                                }
                            }
                        }
                        if (analyzerOk) {
                            analyzerGraph.disconnectInput(analyzer->id, 0);
                            Node *dynamics = analyzerGraph.addNode("dsp.dynamics", 200, 0);
                            std::string why;
                            if (!dynamics) {
                                analyzerOk = false;
                                analyzerWhat = "the Dynamics block is missing";
                            } else {
                                dynamics->setFloat("threshold", 0.0f);
                                dynamics->setFloat("ratio", 1.0f);
                                dynamics->setBool("limiter", false);
                            }
                            if (analyzerOk &&
                                (!analyzerGraph.connect(src->id, 0, dynamics->id, 0, &why) ||
                                 !analyzerGraph.connect(dynamics->id, 0, analyzer->id, 0, &why))) {
                                analyzerOk = false;
                                analyzerWhat = "could not wire the processed analyzer input";
                            }
                            if (analyzerOk) {
                                analyzerGraph.evaluate(analyzerCtx);
                                const AnalysisData *live = analyzer->outputs[0].analysis.get();
                                if (!live || live == precomputed.get() ||
                                    live->frames.size() != 1) {
                                    analyzerOk = false;
                                    analyzerWhat = "the processed input got no live analysis";
                                } else if (analyzer->outputs[1].scalar < 0.1f) {
                                    analyzerOk = false;
                                    analyzerWhat = "the processed input measured silence";
                                }
                            }
                        }
                        if (analyzerOk) {
                            std::printf("  analyzer : input-driven (silent / precomputed / live)\n");
                        } else {
                            result = fail("Spectrum Analyzer input: " + analyzerWhat);
                        }
                    }

                    // Monitor routing: playback follows the Audio Output, so a
                    // disconnected output is silent and a DAC chain renders the
                    // track the monitor streams.
                    if (result == 0) {
                        Project monitor = project;
                        bool monitorOk = true;
                        std::string monitorWhat;
                        if (Exporter::audioRoute(monitor) == AudioRoute::Silent) {
                            monitorOk = false;
                            monitorWhat = "the default route is not wired";
                        }
                        Node *monitorOut = nullptr;
                        for (Node &node : monitor.graph.nodes) {
                            if (node.kind == "out.audio") monitorOut = &node;
                        }
                        if (!monitorOut) {
                            monitorOk = false;
                            monitorWhat = "the default project has no Audio Output";
                        } else {
                            monitor.graph.disconnectInput(monitorOut->id, 0);
                            if (Exporter::audioRoute(monitor) != AudioRoute::Silent) {
                                monitorOk = false;
                                monitorWhat = "a disconnected Audio Output is not silent";
                            }
                            Node *constant = monitor.graph.addNode("math.constant", 0, 200);
                            Node *dac = monitor.graph.addNode("dsp.dac", 200, 200);
                            std::string why;
                            if (!constant || !dac ||
                                !monitor.graph.connect(constant->id, 0, dac->id, 0, &why) ||
                                !monitor.graph.connect(dac->id, 0, monitorOut->id, 0, &why)) {
                                monitorOk = false;
                                monitorWhat = "could not wire the DAC route: " + why;
                            } else {
                                constant->setFloat("value", 0.5f);
                                if (Exporter::audioRoute(monitor) != AudioRoute::Processed) {
                                    monitorOk = false;
                                    monitorWhat = "a DAC chain is not the processed route";
                                }
                                AudioPtr rendered;
                                std::string renderError;
                                if (!Exporter::renderOutputAudio(monitor, clip.buffer(), analysis,
                                                                 0.0, 1.0, {}, {}, &rendered,
                                                                 &renderError)) {
                                    monitorOk = false;
                                    monitorWhat = "monitor render: " + renderError;
                                } else if (!rendered || rendered->frameCount != 48000 ||
                                           std::fabs(rendered->samples[24000] - 0.5f) > 0.01f) {
                                    monitorOk = false;
                                    monitorWhat = "monitor render produced the wrong samples";
                                } else {
                                    clip.setPlaybackBuffer(rendered);
                                    clip.seek(0.0);
                                    clip.startPreview();
                                    const bool played = clip.previewPlaying();
                                    clip.stopPreview();
                                    clip.setPlaybackBuffer(nullptr);
                                    clip.seek(0.0);
                                    clip.startPreview();
                                    const bool silent = !clip.previewPlaying();
                                    clip.clearPlaybackBuffer();
                                    clip.startLiveStream(48000, 2, 512);
                                    AudioBuffer window;
                                    window.channels = 2;
                                    window.sampleRate = 48000;
                                    window.frameCount = 8192;
                                    window.samples.assign(8192 * 2, 0.05f);
                                    clip.pushLiveWindow(window);
                                    const bool livePlaying = clip.previewPlaying();
                                    clip.stopLiveStream();
                                    if (!played || !silent || !livePlaying) {
                                        monitorOk = false;
                                        monitorWhat = "the monitor did not follow the override";
                                    }
                                }
                            }
                        }
                        if (monitorOk) {
                            std::printf("  monitor  : source/silent/processed routing ok\n");
                        } else {
                            result = fail("monitor routing: " + monitorWhat);
                        }
                    }

                    // A Unity ADC wired straight into a DAC copies the imported
                    // clip sample for sample, so the export muxes the original
                    // file and playback uses the decoded clip instead of
                    // rendering and re-encoding the whole audio-rate region.
                    if (result == 0) {
                        bool bridgeOk = true;
                        std::string bridgeWhat;
                        Project bridge;
                        bridge.audio.path = "track.flac";
                        bridge.audio.channels = 2;
                        bridge.audio.sampleRate = 48000;
                        bridge.audio.peak = 0.8f;
                        const int srcId = bridge.graph.addNode("src.audio", 0, 0)->id;
                        const int adcId = bridge.graph.addNode("dsp.adc", 200, 0)->id;
                        const int dacId = bridge.graph.addNode("dsp.dac", 400, 0)->id;
                        const int outId = bridge.graph.addNode("out.audio", 600, 0)->id;
                        std::string why;
                        bridgeOk = bridge.graph.connect(srcId, 0, adcId, 0, &why) &&
                                   bridge.graph.connect(adcId, 0, dacId, 0, &why) &&
                                   bridge.graph.connect(adcId, 1, dacId, 1, &why) &&
                                   bridge.graph.connect(dacId, 0, outId, 0, &why);
                        if (!bridgeOk) bridgeWhat = "could not wire the bridge: " + why;
                        if (bridgeOk && Exporter::audioRoute(bridge) != AudioRoute::Source) {
                            bridgeOk = false;
                            bridgeWhat = "a plain ADC -> DAC bridge is not treated as the source";
                        }
                        if (bridgeOk) {
                            // A meter tapping the bridge only reads it, so the
                            // audio is still the imported clip.
                            const int meterId = bridge.graph.addNode("dbg.meter", 200, 200)->id;
                            bridge.graph.connect(adcId, 0, meterId, 0, &why);
                            if (Exporter::audioRoute(bridge) != AudioRoute::Source) {
                                bridgeOk = false;
                                bridgeWhat = "a meter tap broke the passthrough route";
                            }
                            bridge.graph.removeNode(meterId);
                        }
                        if (bridgeOk) {
                            // Meters *inline* between the ADC and the DAC are
                            // transparent too, which is how the common
                            // ADC -> meters -> DAC chain is wired.
                            const int inlineId = bridge.graph.addNode("dbg.meter", 200, 300)->id;
                            bridge.graph.disconnectInput(dacId, 0);
                            bridge.graph.connect(adcId, 0, inlineId, 0, &why);
                            bridge.graph.connect(inlineId, 0, dacId, 0, &why);
                            if (Exporter::audioRoute(bridge) != AudioRoute::Source) {
                                bridgeOk = false;
                                bridgeWhat = "an inline meter broke the passthrough route";
                            }
                            bridge.graph.disconnectInput(dacId, 0);
                            bridge.graph.removeNode(inlineId);
                            bridge.graph.connect(adcId, 0, dacId, 0, &why);
                        }
                        Node *adc = bridge.graph.find(adcId);
                        Node *dac = bridge.graph.find(dacId);
                        if (bridgeOk) {
                            // Anything that can change a sample ends the shortcut.
                            const int mathId = bridge.graph.addNode("math.arithmetic", 300, 200)->id;
                            bridge.graph.disconnectInput(dacId, 0);
                            bridge.graph.connect(adcId, 0, mathId, 0, &why);
                            bridge.graph.connect(mathId, 0, dacId, 0, &why);
                            if (Exporter::audioRoute(bridge) != AudioRoute::Processed) {
                                bridgeOk = false;
                                bridgeWhat = "a processor in the path is treated as a passthrough";
                            }
                            bridge.graph.disconnectInput(dacId, 0);
                            bridge.graph.removeNode(mathId);
                            bridge.graph.connect(adcId, 0, dacId, 0, &why);
                        }
                        if (bridgeOk) {
                            adc->setInt("mode", 1);  // RMS is a measurement, not a copy
                            if (Exporter::audioRoute(bridge) != AudioRoute::Processed) {
                                bridgeOk = false;
                                bridgeWhat = "an RMS ADC is treated as a passthrough";
                            }
                            adc->setInt("mode", 0);
                        }
                        if (bridgeOk) {
                            bridge.audio.channels = 1;  // the layouts no longer match
                            if (Exporter::audioRoute(bridge) != AudioRoute::Processed) {
                                bridgeOk = false;
                                bridgeWhat = "a channel mismatch is treated as a passthrough";
                            }
                            bridge.audio.channels = 2;
                        }
                        if (bridgeOk) {
                            bridge.audio.peak = 1.2f;  // the DAC clamp would bite
                            if (Exporter::audioRoute(bridge) != AudioRoute::Processed) {
                                bridgeOk = false;
                                bridgeWhat = "a hot clip is treated as a passthrough";
                            }
                            dac->setBool("clamp", false);
                            if (Exporter::audioRoute(bridge) != AudioRoute::Source) {
                                bridgeOk = false;
                                bridgeWhat = "an unclamped bridge is not treated as the source";
                            }
                        }
                        if (!bridgeOk) {
                            result = fail("passthrough bridge: " + bridgeWhat);
                        } else {
                            std::printf("  routing  : a plain ADC -> DAC bridge uses the original "
                                        "audio\n");
                        }
                    }

                    // A Signal Filter between the ADC and the DAC runs at the
                    // audio rate, so its in-block response plot (and its pivot)
                    // must use that rate's Nyquist instead of the frame rate.
                    // The block publishes the rate it ran with for the canvas.
                    if (result == 0) {
                        Graph filterGraph;
                        const int srcId = filterGraph.addNode("src.audio", 0, 0)->id;
                        const int adcId = filterGraph.addNode("dsp.adc", 200, 0)->id;
                        const int filterId = filterGraph.addNode("mod.filter", 350, 0)->id;
                        const int dacId = filterGraph.addNode("dsp.dac", 500, 0)->id;
                        const int outId = filterGraph.addNode("out.audio", 700, 0)->id;
                        std::string why;
                        bool filterOk = filterGraph.connect(srcId, 0, adcId, 0, &why) &&
                                        filterGraph.connect(adcId, 0, filterId, 0, &why) &&
                                        filterGraph.connect(filterId, 0, dacId, 0, &why) &&
                                        filterGraph.connect(adcId, 1, dacId, 1, &why) &&
                                        filterGraph.connect(dacId, 0, outId, 0, &why);
                        std::string filterWhat;
                        if (!filterOk) filterWhat = "could not wire the filter region: " + why;
                        if (filterOk) {
                            EvalContext ctx;
                            ctx.fps = 60.0f;
                            ctx.duration = 1.0;
                            ctx.audio = clip.buffer();
                            ctx.audioSampleRate = 48000;
                            ctx.frame = 0;
                            ctx.time = 0.0;
                            ctx.audioTime = 0.0;
                            filterGraph.evaluate(ctx);
                            Node *node = filterGraph.find(filterId);
                            const double rate =
                                node ? node->runtimeState["filter.rate"] : 0.0;
                            if (std::fabs(rate - 48000.0) > 0.5) {
                                filterOk = false;
                                filterWhat = "the filter published the frame rate (" +
                                             std::to_string(rate) + ")";
                            }
                        }
                        // Outside a region it still follows the frame rate.
                        if (filterOk) {
                            Graph plain;
                            const int sourceId = plain.addNode("math.constant", 0, 0)->id;
                            const int plainFilter = plain.addNode("mod.filter", 200, 0)->id;
                            if (plain.connect(sourceId, 0, plainFilter, 0, &why)) {
                                EvalContext ctx;
                                ctx.fps = 60.0f;
                                ctx.duration = 1.0;
                                plain.evaluate(ctx);
                                Node *node = plain.find(plainFilter);
                                const double rate =
                                    node ? node->runtimeState["filter.rate"] : 0.0;
                                if (std::fabs(rate - 60.0) > 0.5) {
                                    filterOk = false;
                                    filterWhat = "the filter lost the frame rate outside a region";
                                }
                            }
                        }
                        if (!filterOk) {
                            result = fail("filter visual rate: " + filterWhat);
                        } else {
                            std::printf("  filter   : the response plot follows the audio rate in "
                                        "an ADC -> DAC region\n");
                        }
                    }

                    // A processed Analysis input must measure the same window in
                    // preview and in export. The analyzer keeps its own history of
                    // what reaches its port, so a one-video-frame window is not
                    // zero-padded into a spuriously different reading; this is the
                    // Frequency Band modulation mismatch between preview and
                    // export.
                    if (result == 0) {
                        auto buildProcessed = [&](int *analyzerOut) {
                            auto graph = std::make_shared<Graph>();
                            const int srcId = graph->addNode("src.audio", 0.0f, 0.0f)->id;
                            const int adcId = graph->addNode("dsp.adc", 200.0f, 0.0f)->id;
                            const int dacId = graph->addNode("dsp.dac", 400.0f, 0.0f)->id;
                            const int outId = graph->addNode("out.audio", 600.0f, 0.0f)->id;
                            const int analyzerId = graph->addNode("dsp.analyze", 400.0f, 160.0f)->id;
                            *analyzerOut = analyzerId;
                            std::string why;
                            if (!graph->connect(srcId, 0, adcId, 0, &why) ||
                                !graph->connect(adcId, 0, dacId, 0, &why) ||
                                !graph->connect(adcId, 1, dacId, 1, &why) ||
                                !graph->connect(dacId, 0, outId, 0, &why) ||
                                !graph->connect(dacId, 0, analyzerId, 0, &why)) {
                                return std::shared_ptr<Graph>{};
                            }
                            return graph;
                        };
                        int previewAnalyzer = 0;
                        int exportAnalyzer = 0;
                        std::shared_ptr<Graph> previewGraph = buildProcessed(&previewAnalyzer);
                        std::shared_ptr<Graph> exportGraph = buildProcessed(&exportAnalyzer);
                        bool windowOk = previewGraph && exportGraph;
                        std::string windowWhat;
                        if (!windowOk) {
                            windowWhat = "could not wire the processed analysis chain";
                        }
                        const auto levelAfter = [&](Graph &graph, int analyzerId, bool offline,
                                                    int frames) {
                            EvalContext ctx;
                            ctx.fps = 60.0f;
                            ctx.duration = 1.0;
                            ctx.audio = clip.buffer();
                            ctx.analysis = analysis;
                            ctx.offline = offline;
                            float level = 0.0f;
                            for (int frame = 0; frame < frames; ++frame) {
                                ctx.frame = frame;
                                ctx.time = frame / 60.0;
                                ctx.audioTime = frame / 60.0;
                                graph.evaluate(ctx);
                                if (const Node *node = graph.find(analyzerId)) {
                                    if (node->outputs.size() > 1) level = node->outputs[1].scalar;
                                }
                            }
                            return level;
                        };
                        if (windowOk) {
                            const float previewLevel =
                                levelAfter(*previewGraph, previewAnalyzer, false, 10);
                            const float exportLevel =
                                levelAfter(*exportGraph, exportAnalyzer, true, 10);
                            if (std::fabs(previewLevel - exportLevel) > 0.02f) {
                                windowOk = false;
                                windowWhat = "preview level " + std::to_string(previewLevel) +
                                             " vs export " + std::to_string(exportLevel);
                            }
                        }
                        if (!windowOk) {
                            result = fail("processed analysis window: " + windowWhat);
                        } else {
                            std::printf("  analyzer : preview and export measure the same window\n");
                        }
                    }

                    // An export must not leave whole-track render buffers behind:
                    // the preview's window reuse would accept them and the monitor
                    // would never be handed a new sample again, which silenced
                    // playback after an export.
                    if (result == 0) {
                        Project exported = project;
                        const int sink = exported.graph.audioSinkNodeId();
                        const Link *sinkLink =
                            sink > 0 ? exported.graph.findInputLink(sink, 0) : nullptr;
                        Node *dac = sinkLink ? exported.graph.find(sinkLink->fromNode) : nullptr;
                        AudioPtr renderedTrack;
                        std::string renderError;
                        bool exportOk =
                            dac && Exporter::renderOutputAudio(exported, clip.buffer(), analysis,
                                                               0.0, 0.5, {}, {}, &renderedTrack,
                                                               &renderError);
                        if (!exportOk) {
                            result = fail("post-export playback: " + renderError);
                        } else {
                            EvalContext ctx;
                            ctx.fps = 60.0f;
                            ctx.duration = 1.0;
                            ctx.audio = clip.buffer();
                            ctx.analysis = analysis;
                            ctx.offline = false;
                            ctx.frame = 20;
                            ctx.time = 20.0 / 60.0;
                            ctx.audioTime = ctx.time;
                            exported.graph.evaluate(ctx);
                            // One video frame of samples at the preview's rate,
                            // not the export's whole-track buffer.
                            if (!dac->audioRenderOutput || dac->audioRenderFrames != 800) {
                                result = fail("post-export playback: the preview reused the stale "
                                              "export buffer");
                            } else {
                                std::printf("  export   : playback starts from a fresh window "
                                            "afterwards\n");
                            }
                        }
                    }

                    // Modulation inputs on the existing blocks. They all follow
                    // the same pattern (rates in octaves, levels scaled, offsets
                    // additive), so one check per block covers the wiring.
                    if (result == 0) {
                        bool modOk = true;
                        std::string modWhat;

                        // LFO: a +1 frequency input doubles the rate, and
                        // sin(2pi*0.25)=1 while sin(2pi*0.5)=0.
                        auto lfoAt = [&](bool modulated, float *value) {
                            Graph graph;
                            Node *source = graph.addNode("math.constant", 0, 0);
                            Node *lfo = graph.addNode("mod.lfo", 200, 0);
                            if (!source || !lfo) return false;
                            lfo->setInt("shape", 0);
                            lfo->setFloat("frequency", 1.0f);
                            source->setFloat("value", 1.0f);
                            if (modulated) graph.connect(source->id, 0, lfo->id, 1);
                            EvalContext ctx;
                            ctx.fps = 60.0f;
                            ctx.time = 0.25;
                            graph.evaluate(ctx);
                            *value = lfo->outputs[0].scalar;
                            return true;
                        };
                        float plainLfo = 0.0f, modulatedLfo = 0.0f;
                        if (!lfoAt(false, &plainLfo) || !lfoAt(true, &modulatedLfo)) {
                            modOk = false;
                            modWhat = "the LFO block is missing";
                        } else if (std::fabs(plainLfo - 1.0f) > 0.02f ||
                                   std::fabs(modulatedLfo) > 0.02f) {
                            modOk = false;
                            modWhat = "the LFO frequency input did not shift the rate";
                        }

                        // Beat Pulse: a +1 tempo input doubles the BPM, so the beat
                        // must land at half the period. Measure the period the block
                        // actually fires at - scan time for the next beat (the pulse
                        // decays towards zero between beats and jumps back up to 1.0
                        // on one) - instead of pinning one absolute beat length: the
                        // block owns the constant that maps BPM x division to seconds
                        // and it may be retuned.
                        auto pulsePeriod = [&](float tempoInput, double *period) {
                            Graph graph;
                            Node *source = graph.addNode("math.constant", 0, 0);
                            Node *pulse = graph.addNode("time.pulse", 200, 0);
                            if (!source || !pulse) return false;
                            pulse->setFloat("bpm", 120.0f);
                            pulse->setInt("division", 2);   // 1/4
                            pulse->setFloat("decay", 0.5f);
                            source->setFloat("value", tempoInput);
                            graph.connect(source->id, 0, pulse->id, 0);
                            EvalContext ctx;
                            ctx.fps = 60.0f;
                            const double step = 1.0 / 4000.0;   // 0.25 ms
                            double previousValue = -1.0;
                            double previousTime = 0.0;
                            for (int i = 1; i <= 8000; ++i) {   // up to 2 s
                                ctx.time = step * i;
                                graph.evaluate(ctx);
                                const double value = pulse->outputs[0].scalar;
                                if (previousValue >= 0.0 && value - previousValue > 0.1) {
                                    // A beat landed inside (previousTime, time].
                                    *period = 0.5 * (previousTime + ctx.time);
                                    return true;
                                }
                                previousValue = value;
                                previousTime = ctx.time;
                            }
                            return false;
                        };
                        double plainPulse = 0.0, modulatedPulse = 0.0;
                        const bool pulseFound = modOk && pulsePeriod(0.0f, &plainPulse) &&
                                                pulsePeriod(1.0f, &modulatedPulse);
                        if (modOk && !pulseFound) {
                            modOk = false;
                            modWhat = "the Beat Pulse block is missing or never fires";
                        } else if (modOk && std::fabs(plainPulse - 2.0 * modulatedPulse) >
                                                   0.05 * plainPulse) {
                            modOk = false;
                            modWhat = "the Beat Pulse tempo input did not retime the pulse (" +
                                      std::to_string(plainPulse) + " s vs " +
                                      std::to_string(modulatedPulse) + " s)";
                        }

                        // Signal Filter: a +2 cutoff input opens the filter, so a
                        // step settles faster than the unmodulated one.
                        auto filterStepAt = [&](float cutoffInput, float *value) {
                            Graph graph;
                            Node *signal = graph.addNode("math.constant", 0, 0);
                            Node *cutoff = graph.addNode("math.constant", 0, 60);
                            Node *filter = graph.addNode("mod.filter", 200, 0);
                            if (!signal || !cutoff || !filter) return false;
                            signal->setFloat("value", 1.0f);
                            cutoff->setFloat("value", cutoffInput);
                            filter->setInt("mode", 0);
                            filter->setFloat("cutoff", 2.0f);
                            filter->setFloat("resonance", 0.707f);
                            graph.connect(signal->id, 0, filter->id, 0);
                            graph.connect(cutoff->id, 0, filter->id, 1);
                            EvalContext ctx;
                            ctx.fps = 60.0f;
                            // 8 frames: the 2 Hz filter is still on its way up while
                            // the 8 Hz one has all but settled.
                            for (int i = 0; i < 8; ++i) graph.evaluate(ctx);
                            *value = filter->outputs[0].scalar;
                            // The block publishes what it used so the inspector
                            // slider and the response plot can follow it.
                            float published = 0.0f;
                            if (!filter->effectiveParam("cutoff", &published)) return false;
                            const float expected =
                                cutoffInput == 0.0f ? 2.0f : 2.0f * std::pow(2.0f, cutoffInput);
                            if (std::fabs(published - expected) > 0.01f) return false;
                            return true;
                        };
                        float plainCutoff = 0.0f, modulatedCutoff = 0.0f;
                        if (modOk && (!filterStepAt(0.0f, &plainCutoff) ||
                                      !filterStepAt(2.0f, &modulatedCutoff))) {
                            modOk = false;
                            modWhat = "the Signal Filter block is missing";
                        } else if (modOk && modulatedCutoff < plainCutoff + 0.05f) {
                            modOk = false;
                            modWhat = "the Signal Filter cutoff input did nothing";
                        }

                        // Ringbuffer: a +4 speed input advances the read pointer.
                        auto ringPhase = [&](bool modulated, double *phase) {
                            Graph graph;
                            Node *signal = graph.addNode("math.constant", 0, 0);
                            Node *speed = graph.addNode("math.constant", 0, 60);
                            Node *ring = graph.addNode("mod.ringbuffer", 200, 0);
                            if (!signal || !speed || !ring) return false;
                            signal->setFloat("value", 0.25f);
                            speed->setFloat("value", 4.0f);   // x16
                            ring->setFloat("speed", 0.05f);
                            graph.connect(signal->id, 0, ring->id, 0);
                            if (modulated) graph.connect(speed->id, 0, ring->id, 1);
                            EvalContext ctx;
                            ctx.fps = 60.0f;
                            graph.evaluate(ctx);
                            *phase = ring->runtimeState["phase"];
                            return true;
                        };
                        double plainPhase = 0.0, modulatedPhase = 0.0;
                        if (modOk && (!ringPhase(false, &plainPhase) ||
                                      !ringPhase(true, &modulatedPhase))) {
                            modOk = false;
                            modWhat = "the Ringbuffer block is missing";
                        } else if (modOk && modulatedPhase <= plainPhase) {
                            modOk = false;
                            modWhat = "the Ringbuffer speed input did nothing";
                        }

                        // Automation: the offset input shifts the output.
                        if (modOk) {
                            Graph automationGraph;
                            Node *automation = automationGraph.addNode("mod.automation", 0, 0);
                            Node *offsetSource = automationGraph.addNode("math.constant", 200, 0);
                            if (!automation || !offsetSource) {
                                modOk = false;
                                modWhat = "the Automation block is missing";
                            } else {
                                if (Param *curve = automation->find("curve")) {
                                    curve->keys = {Keyframe{0.0, 0.0f, 0},
                                                   Keyframe{1.0, 0.0f, 0}};
                                }
                                automation->setFloat("depth", 1.0f);
                                automation->setFloat("offset", 0.0f);
                                offsetSource->setFloat("value", 0.75f);
                                automationGraph.connect(offsetSource->id, 0, automation->id, 1);
                                EvalContext ctx;
                                ctx.fps = 60.0f;
                                ctx.time = 1.0;
                                ctx.duration = 4.0;
                                automationGraph.evaluate(ctx);
                                if (std::fabs(automation->outputs[0].scalar - 0.75f) > 0.01f) {
                                    modOk = false;
                                    modWhat = "the Automation offset input did nothing";
                                }
                            }
                        }

                        if (modOk) {
                            std::printf("  modports : LFO frequency, Beat tempo, filter cutoff, "
                                        "ring speed and automation offset ok\n");
                        } else {
                            result = fail("modulation inputs: " + modWhat);
                        }
                    }
                }

                // Automation Slice mode: the curve only covers the Start..
                // Finish window, holds its first/last value outside it and
                // mirrors when Start is moved past Finish.
                if (result == 0) {
                    bool sliceOk = true;
                    std::string sliceWhat;
                    Graph sliceGraph;
                    Node *automation = sliceGraph.addNode("mod.automation", 0, 0);
                    if (!automation) {
                        sliceOk = false;
                        sliceWhat = "the Automation block is missing";
                    } else {
                        if (Param *curve = automation->find("curve")) {
                            curve->keys = {Keyframe{0.0, 0.25f, 0},
                                           Keyframe{1.0, 0.75f, 0}};
                        }
                        automation->setBool("slice", true);
                        automation->setBool("loop", false);
                        automation->setBool("smooth", false);
                        automation->setFloat("depth", 1.0f);
                        automation->setFloat("offset", 0.0f);
                        automation->setFloat("start", 1.0f);
                        automation->setFloat("finish", 3.0f);
                        const auto valueAt = [&](double time) {
                            EvalContext ctx;
                            ctx.fps = 60.0f;
                            ctx.time = time;
                            ctx.duration = 8.0;
                            sliceGraph.evaluate(ctx);
                            return automation->outputs[0].scalar;
                        };
                        const float before = valueAt(0.5);  // before Start: first value
                        const float middle = valueAt(2.0);  // halfway through the slice
                        const float after = valueAt(5.0);   // after Finish: last value
                        automation->setFloat("start", 3.0f);
                        automation->setFloat("finish", 1.0f);
                        const float reversedEarly = valueAt(1.0);  // the Finish end
                        const float reversedLate = valueAt(3.0f);  // the Start end
                        if (std::fabs(before - 0.25f) > 0.01f ||
                            std::fabs(middle - 0.5f) > 0.01f ||
                            std::fabs(after - 0.75f) > 0.01f ||
                            std::fabs(reversedEarly - 0.75f) > 0.01f ||
                            std::fabs(reversedLate - 0.25f) > 0.01f) {
                            sliceOk = false;
                            sliceWhat = "slice values are wrong (" + std::to_string(before) +
                                        " " + std::to_string(middle) + " " +
                                        std::to_string(after) + " / " +
                                        std::to_string(reversedEarly) + " " +
                                        std::to_string(reversedLate) + ")";
                        }
                        if (sliceOk) {
                            std::printf("  modports : Automation slice holds, maps and mirrors "
                                        "(%.2f %.2f %.2f / %.2f %.2f)\n",
                                        before, middle, after, reversedEarly, reversedLate);
                        }
                    }
                    if (!sliceOk) result = fail("Automation slice: " + sliceWhat);
                }
            }

            // --- a real GPU export when the machine can do it --------------
            if (result == 0 && ffmpeg::canRunVideoEncoder("h264_nvenc")) {
                Project gpu = project;
                gpu.output.container = "mp4";
                gpu.output.videoCodec = "h264_nvenc";
                gpu.video.width = 640;
                gpu.video.height = 360;
                gpu.video.useAudioDuration = false;
                gpu.video.duration = 1.0;
                ExportRequest request;
                request.outputPath = "selftest_nvenc.mp4";
                request.overwrite = true;
                request.audioSampleRate = 48000;
                std::string gpuError;
                const bool gpuOk = Exporter::run(renderer, gpu, request, clip.buffer(), analysis,
                                                nullptr, nullptr, &gpuError);
                if (!gpuOk) {
                    result = fail("NVENC export: " + gpuError);
                } else {
                    const MediaInfo gpuInfo = ffmpeg::probe("selftest_nvenc.mp4");
                    if (!gpuInfo.ok || gpuInfo.videoCodec != "h264") {
                        result = fail("NVENC export produced " +
                                      (gpuInfo.ok ? gpuInfo.videoCodec : std::string("nothing")));
                    } else {
                        std::printf("  gpu      : NVENC export ok (%s + %s, %.2f s)\n",
                                    gpuInfo.videoCodec.c_str(), gpuInfo.codec.c_str(),
                                    gpuInfo.duration);
                    }
                }
            }

            // --- export the real thing ------------------------------------
            // --- single line text editing ---------------------------------
            if (result == 0) {
                auto run = [](TextEditState &edit, const TextEditKeys &keys) {
                    return applyTextEditKeys(edit, keys);
                };
                bool ok = true;
                std::string what;

                TextEditState arrows;
                arrows.begin("hello world");
                TextEditKeys keys;
                keys.left = true;
                run(arrows, keys);
                if (arrows.caret() != 10) {
                    ok = false;
                    what = "left arrow";
                }
                keys = TextEditKeys{};
                keys.home = true;
                run(arrows, keys);
                if (arrows.caret() != 0) {
                    ok = false;
                    what = "home";
                }
                keys = TextEditKeys{};
                keys.end = true;
                run(arrows, keys);
                if (arrows.caret() != 11) {
                    ok = false;
                    what = "end";
                }

                // SHIFT+HOME selects everything and typing replaces it.
                keys = TextEditKeys{};
                keys.home = true;
                keys.shift = true;
                run(arrows, keys);
                if (!arrows.hasSelection() || arrows.selection() != "hello world") {
                    ok = false;
                    what = "shift+home selection";
                }
                keys = TextEditKeys{};
                keys.typed = "bye";
                run(arrows, keys);
                if (arrows.text() != "bye" || arrows.caret() != 3) {
                    ok = false;
                    what = "typing over a selection";
                }

                // Mouse-style placement plus shift extension in both directions.
                arrows.setCaret(1, false);
                arrows.setCaret(3, true);
                if (arrows.selection() != "ye") {
                    ok = false;
                    what = "shift extension";
                }

                // Word jumps and word deletion (Ctrl+arrows / Ctrl+backspace).
                if (textIndexWordLeft("alpha beta gamma", 16) != 11 ||
                    textIndexWordLeft("alpha beta gamma", 11) != 6 ||
                    textIndexWordRight("alpha beta gamma", 0) != 5) {
                    ok = false;
                    what = "word jumps";
                }
                TextEditState words;
                words.begin("alpha beta");
                keys = TextEditKeys{};
                keys.backspace = true;
                keys.ctrl = true;
                run(words, keys);
                if (words.text() != "alpha ") {
                    ok = false;
                    what = "ctrl+backspace";
                }

                // Backspace and Delete at the caret.
                TextEditState erase;
                erase.begin("abc");
                erase.setCaret(1, false);
                erase.erase(false);
                if (erase.text() != "bc") {
                    ok = false;
                    what = "backspace";
                }
                erase.setCaret(0, false);
                erase.erase(true);
                if (erase.text() != "c") {
                    ok = false;
                    what = "delete";
                }

                // Cut / paste round trip.
                TextEditState clip;
                clip.begin("copy me");
                clip.selectAll();
                keys = TextEditKeys{};
                keys.cut = true;
                const TextEditApplied cut = run(clip, keys);
                if (!cut.copied || cut.clipboard != "copy me" || !clip.text().empty()) {
                    ok = false;
                    what = "cut";
                }
                keys = TextEditKeys{};
                keys.paste = true;
                keys.clipboard = cut.clipboard;
                run(clip, keys);
                if (clip.text() != "copy me" || clip.caret() != 7) {
                    ok = false;
                    what = "paste";
                }

                // Enter keeps the typed text, Escape puts the old value back.
                TextEditState finish;
                finish.begin("start");
                keys = TextEditKeys{};
                keys.typed = "!";
                run(finish, keys);
                keys = TextEditKeys{};
                keys.commit = true;
                const TextEditApplied committed = run(finish, keys);
                if (!committed.finished || !committed.commit || finish.text() != "start!") {
                    ok = false;
                    what = "enter commit";
                }
                // The widget ends the edit on commit; the next edit starts from
                // the committed value, and Escape restores that value.
                finish.begin(finish.text());
                keys = TextEditKeys{};
                keys.typed = "XX";
                run(finish, keys);
                keys = TextEditKeys{};
                keys.cancel = true;
                const TextEditApplied cancelled = run(finish, keys);
                if (!cancelled.finished || cancelled.commit || finish.text() != "start!") {
                    ok = false;
                    what = "escape restore";
                }

                // Undo: a run of typed characters is one step, a discrete edit
                // is another, and Ctrl+Z walks back through them.
                if (ok) {
                    TextEditState undoState;
                    undoState.begin("");
                    for (const char *ch : {"a", "b", "c"}) {
                        keys = TextEditKeys{};
                        keys.typed = ch;
                        run(undoState, keys);
                    }
                    keys = TextEditKeys{};
                    keys.undo = true;
                    run(undoState, keys);
                    if (undoState.text() != "" || undoState.caret() != 0) {
                        ok = false;
                        what = "ctrl+z did not undo the typing run";
                    }
                }

                // Multi-line editing: Enter breaks the line and keeps editing,
                // pasted text keeps its newlines, and the single-line field
                // still strips them.
                if (ok) {
                    TextEditState area;
                    area.begin("Hello");
                    area.setCaret(5, false);
                    keys = TextEditKeys{};
                    keys.commit = true;
                    keys.allowNewlines = true;
                    const TextEditApplied broken = run(area, keys);
                    if (broken.finished || area.text() != "Hello\n" || area.caret() != 6) {
                        ok = false;
                        what = "enter did not break the line in a multi-line editor";
                    }
                }
                if (ok) {
                    TextEditState area;
                    area.begin("");
                    keys = TextEditKeys{};
                    keys.paste = true;
                    keys.allowNewlines = true;
                    keys.clipboard = "one\ntwo\n";
                    run(area, keys);
                    if (area.text() != "one\ntwo\n") {
                        ok = false;
                        what = "a multi-line paste lost its line breaks";
                    }
                    TextEditState field;
                    field.begin("");
                    keys = TextEditKeys{};
                    keys.paste = true;
                    keys.clipboard = "one\ntwo";
                    run(field, keys);
                    if (field.text() != "onetwo") {
                        ok = false;
                        what = "a single-line field kept a pasted newline";
                    }
                }
                if (ok) {
                    // A line break is its own undo step, so Ctrl+Z after typing
                    // on the new line restores the break first.
                    TextEditState area;
                    area.begin("one");
                    area.setCaret(3, false);
                    keys = TextEditKeys{};
                    keys.commit = true;
                    keys.allowNewlines = true;
                    run(area, keys);
                    keys = TextEditKeys{};
                    keys.typed = "two";
                    run(area, keys);
                    keys = TextEditKeys{};
                    keys.undo = true;
                    run(area, keys);
                    if (area.text() != "one\n") {
                        ok = false;
                        what = "undo did not stop at the line break";
                    }
                }

                if (ok) {
                    std::printf("  textedit : arrows, home/end, shift selection, replace, word "
                                "jumps, cut/paste, enter/escape, undo, multi-line ok\n");
                } else {
                    result = fail("text editing: " + what);
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

            // --- Dynamics reaches the exported audio ----------------------
            if (result == 0) {
                Project dynProject = project;
                dynProject.video.width = 320;
                dynProject.video.height = 180;
                Graph &graph = dynProject.graph;
                Node *source = nullptr;
                Node *analyzer = nullptr;
                Node *audioOut = nullptr;
                for (Node &node : graph.nodes) {
                    if (node.kind == "src.audio") source = &node;
                    if (node.kind == "dsp.analyze") analyzer = &node;
                    if (node.kind == "out.audio") audioOut = &node;
                }
                if (!source || !analyzer || !audioOut) {
                    result = fail(
                        "dynamics export: the default pipeline is missing source/analyzer/audio "
                        "output");
                } else {
                    graph.disconnectInput(analyzer->id, 0);
                    Node *dynamics = graph.addNode("dsp.dynamics", 0, 0);
                    if (!dynamics) {
                        result = fail("dynamics export: could not add the block");
                    } else {
                        dynamics->setInt("mode", 0);            // compress
                        dynamics->setFloat("threshold", -18.0f);
                        dynamics->setFloat("ratio", 4.0f);
                        // A fast attack lets the peak detector track the sine's
                        // envelope closely, so the steady-state result matches
                        // the static curve instead of the detector's own lag.
                        dynamics->setFloat("attack", 0.1f);
                        dynamics->setFloat("release", 100.0f);
                        dynamics->setBool("limiter", true);
                        std::string why;
                        const bool wired =
                            graph.connect(source->id, 0, dynamics->id, 0, &why) &&
                            graph.connect(dynamics->id, 0, analyzer->id, 0, &why) &&
                            graph.connect(dynamics->id, 0, audioOut->id, 0, &why);
                        if (!wired) {
                            result = fail("dynamics export: " + why);
                        } else {
                            // The block and its connections have to survive a
                            // project round trip like every other block.
                            std::string roundTripError;
                            Project reloaded;
                            if (!dynProject.save("selftest_dynamics.pforge", &roundTripError) ||
                                !reloaded.load("selftest_dynamics.pforge", &roundTripError)) {
                                result = fail("dynamics project round trip: " + roundTripError);
                            } else {
                                const Node *reloadedDynamics = nullptr;
                                for (const Node &node : reloaded.graph.nodes) {
                                    if (node.kind == "dsp.dynamics") reloadedDynamics = &node;
                                }
                                if (!reloadedDynamics ||
                                    std::fabs(reloadedDynamics->pfloat("threshold", 0.0f) + 18.0f) >
                                        0.01f ||
                                    reloaded.graph.links.size() != graph.links.size()) {
                                    result = fail("dynamics project round trip: block or links lost");
                                }
                            }
                        }
                        ExportRequest dynRequest;
                        dynRequest.outputPath = "selftest_dynamics.mp4";
                        dynRequest.overwrite = true;
                        dynRequest.endTime = 1.0;
                        // A deterministic source: 100 Hz at -6 dBFS through a
                        // 4:1 compressor at -18 dBFS has to land 9 dB down, so
                        // the exported RMS is -18 dBFS RMS, not a guess based on
                        // whatever programme material was passed in.
                        auto synthetic = std::make_shared<AudioBuffer>();
                        // Stereo: decodeAudioFloat() upmixes mono to two
                        // channels, which shaves 3 dB off a mono probe.
                        synthetic->channels = 2;
                        synthetic->sampleRate = 48000;
                        synthetic->frameCount = 48000 * 4;
                        synthetic->samples.resize(static_cast<size_t>(synthetic->frameCount) *
                                                  synthetic->channels);
                        for (long long i = 0; i < synthetic->frameCount; ++i) {
                            const float sample =
                                0.5f * std::sin(6.2831853f * 100.0f * i / 48000.0f);
                            synthetic->samples[static_cast<size_t>(i) * 2] = sample;
                            synthetic->samples[static_cast<size_t>(i) * 2 + 1] = sample;
                        }
                        if (result == 0) {
                            std::string dynError;
                            const bool exported =
                                Exporter::run(renderer, dynProject, dynRequest, synthetic,
                                              analysis, {}, []() { return false; }, &dynError);
                            const float sourceDb =
                                loudnessDb(*synthetic, 0, synthetic->frameCount);
                            int exportedChannels = 0;
                            std::vector<float> exportedSamples;
                            std::string decodeError;
                            const bool decoded = ffmpeg::decodeAudioFloat(
                                dynRequest.outputPath, 48000, &exportedChannels, &exportedSamples,
                                &decodeError);
                            if (!exported) {
                                result = fail("dynamics export: " + dynError);
                            } else if (!decoded || exportedSamples.empty()) {
                                result = fail("dynamics export audio: " + decodeError);
                            } else {
                                AudioBuffer exported;
                                exported.channels = std::max(1, exportedChannels);
                                exported.sampleRate = 48000;
                                exported.samples = std::move(exportedSamples);
                                exported.frameCount = static_cast<long long>(exported.samples.size()) /
                                                      exported.channels;
                                // Skip the first 200 ms: the detector needs a
                                // moment to settle, and the transient would hide
                                // the steady-state ratio the test is checking.
                                const float exportedDb = loudnessDb(exported, 9600, 33600);
                                const float expectedDb = -18.0f;
                                if (std::fabs(exportedDb - expectedDb) > 1.5f) {
                                    result = fail("dynamics export: source " +
                                                  std::to_string(sourceDb) + " dBFS, exported " +
                                                  std::to_string(exportedDb) + " dBFS, expected " +
                                                  std::to_string(expectedDb) + " dBFS");
                                } else {
                                    std::printf(
                                        "  dynamics : exported audio %.1f -> %.1f dBFS "
                                        "(graph processed, expected %.1f)\n",
                                        sourceDb, exportedDb, expectedDb);
                                }
                            }
                        }

                        // The Audio Output is exclusive: point it back at the
                        // source and the same project must export the untouched
                        // signal instead of the compressor's output.
                        if (result == 0) {
                            const std::string drySource = "selftest_dry_source.wav";
                            if (!writeSineWav(drySource, 4.0, 48000, 0.5f)) {
                                result = fail("exclusive routing: could not write the source wav");
                            }
                            if (result == 0) {
                                dynProject.audio.path = drySource;
                                graph.disconnectInput(audioOut->id, 0);
                            if (!graph.connect(source->id, 0, audioOut->id, 0, &why)) {
                                result = fail("exclusive routing: " + why);
                            } else {
                                ExportRequest dryRequest = dynRequest;
                                dryRequest.outputPath = "selftest_dry.mp4";
                                std::string dryError;
                                const bool dryExported = Exporter::run(
                                    renderer, dynProject, dryRequest, synthetic, analysis, {},
                                    []() { return false; }, &dryError);
                                int dryChannels = 0;
                                std::vector<float> drySamples;
                                std::string dryDecodeError;
                                const bool dryDecoded = ffmpeg::decodeAudioFloat(
                                    dryRequest.outputPath, 48000, &dryChannels, &drySamples,
                                    &dryDecodeError);
                                if (!dryExported) {
                                    result = fail("exclusive routing: " + dryError);
                                } else if (!dryDecoded || drySamples.empty()) {
                                    result = fail("exclusive routing decode: " + dryDecodeError);
                                } else {
                                    AudioBuffer dry;
                                    dry.channels = std::max(1, dryChannels);
                                    dry.sampleRate = 48000;
                                    dry.samples = std::move(drySamples);
                                    dry.frameCount =
                                        static_cast<long long>(dry.samples.size()) / dry.channels;
                                    const float dryDb = loudnessDb(dry, 9600, 33600);
                                    const float sourceLevel =
                                        loudnessDb(*synthetic, 9600, 33600);
                                    if (std::fabs(dryDb - sourceLevel) > 1.5f) {
                                        result = fail(
                                            "exclusive routing: dry export is " +
                                            std::to_string(dryDb) + " dBFS, source is " +
                                            std::to_string(sourceLevel) + " dBFS");
                                    } else {
                                        std::printf("  routing  : dry Audio Output kept the "
                                                    "source (%.1f dBFS)\n",
                                                    dryDb);
                                    }
                                }
                            }
                            }
                        }

                        // A DAC-driven Audio Output renders the soundtrack from
                        // Scalars: a constant 0.5 has to land at -6 dBFS.
                        if (result == 0) {
                            Node *constant = graph.addNode("math.constant", 0, 140);
                            Node *dac = graph.addNode("dsp.dac", 200, 140);
                            if (!constant || !dac) {
                                result = fail("DAC export: could not add the blocks");
                            } else {
                                constant->setFloat("value", 0.5f);
                                graph.disconnectInput(audioOut->id, 0);
                                std::string dacWhy;
                                if (!graph.connect(constant->id, 0, dac->id, 0, &dacWhy) ||
                                    !graph.connect(dac->id, 0, audioOut->id, 0, &dacWhy)) {
                                    result = fail("DAC export: " + dacWhy);
                                } else {
                                    ExportRequest dacRequest = dynRequest;
                                    dacRequest.outputPath = "selftest_dac.mp4";
                                    std::string dacError;
                                    const bool dacExported = Exporter::run(
                                        renderer, dynProject, dacRequest, synthetic, analysis, {},
                                        []() { return false; }, &dacError);
                                    int dacChannels = 0;
                                    std::vector<float> dacSamples;
                                    std::string dacDecodeError;
                                    const bool dacDecoded = ffmpeg::decodeAudioFloat(
                                        dacRequest.outputPath, 48000, &dacChannels, &dacSamples,
                                        &dacDecodeError);
                                    if (!dacExported) {
                                        result = fail("DAC export: " + dacError);
                                    } else if (!dacDecoded || dacSamples.empty()) {
                                        result = fail("DAC export decode: " + dacDecodeError);
                                    } else {
                                        AudioBuffer rendered;
                                        rendered.channels = std::max(1, dacChannels);
                                        rendered.sampleRate = 48000;
                                        rendered.samples = std::move(dacSamples);
                                        rendered.frameCount = static_cast<long long>(
                                                                 rendered.samples.size()) /
                                                             rendered.channels;
                                        const float dacDb = loudnessDb(rendered, 9600, 33600);
                                        if (std::fabs(dacDb + 6.02f) > 1.5f) {
                                            result = fail("DAC export: rendered " +
                                                          std::to_string(dacDb) +
                                                          " dBFS, expected -6.02 dBFS");
                                        } else {
                                            std::printf("  dac      : exported synthesised "
                                                        "soundtrack (%.1f dBFS)\n",
                                                        dacDb);
                                        }
                                    }
                                }
                            }
                        }

                        // An unconnected Audio Output exports no audio track.
                        if (result == 0) {
                            graph.disconnectInput(audioOut->id, 0);
                            ExportRequest silentRequest = dynRequest;
                            silentRequest.outputPath = "selftest_silent.mp4";
                            silentRequest.endTime = 0.25;
                            std::string silentError;
                            const bool silentExported = Exporter::run(
                                renderer, dynProject, silentRequest, synthetic, analysis, {},
                                []() { return false; }, &silentError);
                            const MediaInfo silentInfo = ffmpeg::probe(silentRequest.outputPath);
                            if (!silentExported) {
                                result = fail("silent route export: " + silentError);
                            } else if (!silentInfo.ok || !silentInfo.codec.empty()) {
                                result = fail("silent route export: the file still carries audio");
                            } else {
                                std::printf("  routing  : unconnected Audio Output exported "
                                            "without a track\n");
                            }
                        }
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
