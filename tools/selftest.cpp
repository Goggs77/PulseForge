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
#include "core/TextEdit.h"
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
                    std::printf("  shader   : derived ports (uPrev/uUser/uColour) and "
                                "shader.pass migration ok\n");
                } else {
                    result = fail("shader block: " + what);
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
                                    "%.3f, biquad lp=%.3f hp=%.3f bp=%.3f\n",
                                    meterSource ? meterSource->pfloat("value") : 0.0f, ringAverage,
                                    lowPass, highPass, bandPass);
                    } else {
                        result = fail("debug/modulation blocks: " + what);
                    }
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

                if (ok) {
                    std::printf("  textedit : arrows, home/end, shift selection, replace, word "
                                "jumps, cut/paste, enter/escape ok\n");
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
            clip.clear();
        }
    }
    renderer.shutdown();

    CloseAudioDevice();
    CloseWindow();
    std::printf(result == 0 ? "self test OK\n" : "self test FAILED\n");
    return result;
}
