// Renders one frame of a pipeline stage and writes it to a PNG, printing the
// mean colour so problems can be narrowed down quickly.
//
//   pf_stage <audio-file> <stage> <out.png> [time-seconds] [w] [h]
//
// stages: 0 shader pass only, 1 + geometry, 2 + post fx, 3 full default project

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "core/Project.h"
#include "dsp/Analysis.h"
#include "dsp/AudioClip.h"
#include "raylib.h"
#include "render/FrameReadback.h"
#include "render/Renderer.h"

using namespace pf;

int main(int argc, char **argv) {
    const std::string audio = argc > 1 ? argv[1] : "selftest_input.wav";
    const int stage = argc > 2 ? std::atoi(argv[2]) : 3;
    const std::string output = argc > 3 ? argv[3] : "stage.png";
    const double time = argc > 4 ? std::atof(argv[4]) : 1.5;
    const int width = argc > 5 ? std::atoi(argv[5]) : 640;
    const int height = argc > 6 ? std::atoi(argv[6]) : 360;

    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(320, 240, "pf_stage");
    InitAudioDevice();

    Renderer renderer;
    std::string error;
    if (!renderer.init(&error)) {
        std::printf("renderer init failed: %s\n", error.c_str());
        return 1;
    }
    AudioClip clip;
    if (!clip.load(audio, 48000, &error)) {
        std::printf("audio load failed: %s\n", error.c_str());
        return 1;
    }
    AnalysisPtr analysis = analyzeAudio(*clip.buffer(), AnalysisSettings{});
    {
        float maxSpectrum = 0.0f, sumSpectrum = 0.0f;
        for (float v : analysis->spectrum) {
            maxSpectrum = std::max(maxSpectrum, v);
            sumSpectrum += v;
        }
        float maxBand = 0.0f, maxLevel = 0.0f, maxOnset = 0.0f;
        for (const auto &frame : analysis->frames) {
            maxLevel = std::max(maxLevel, frame.level);
            maxOnset = std::max(maxOnset, frame.onset);
            for (float v : frame.bands) maxBand = std::max(maxBand, v);
        }
        std::printf("analysis: frames=%zu spectrum max=%.3f mean=%.4f | band max=%.3f "
                    "level max=%.3f onset max=%.3f\n",
                    analysis->frames.size(), maxSpectrum,
                    analysis->spectrum.empty()
                        ? 0.0f
                        : sumSpectrum / static_cast<float>(analysis->spectrum.size()),
                    maxBand, maxLevel, maxOnset);
        const float *row = analysis->spectrumRow(1.5);
        if (row) {
            std::printf("  spectrum @1.5s:");
            for (int i = 0; i < 12; ++i) std::printf(" %.2f", row[i * 20]);
            std::printf("  level=%.3f onset=%.3f\n", analysis->valueAt(1.5, 1),
                        analysis->valueAt(1.5, 3));
        }
    }

    Project project;
    project.resetToDefault();
    project.video.width = width;
    project.video.height = height;
    project.audio.path = audio;
    project.audio.duration = clip.duration();
    Graph &graph = project.graph;

    if (stage == 10) {
        // solid red shader: proves the shader pass renders at all
        graph.clear();
        Node *pass = graph.addNode("shader.pass", 400, 200);
        pass->setInt("preset", 1);  // solid
        pass->setColor("colorA", Color{255, 40, 40, 255});
        Node *output = graph.addNode("out.video", 700, 200);
        graph.connect(pass->id, 0, output->id, 0);
    } else if (stage == 11) {
        // solid red -> geometry (no layer) -> output
        graph.clear();
        Node *pass = graph.addNode("shader.pass", 400, 200);
        pass->setInt("preset", 1);
        pass->setColor("colorA", Color{255, 40, 40, 255});
        Node *geometry = graph.addNode("geom.primitives", 650, 200);
        geometry->setInt("shape", 0);  // circle
        geometry->setColor("colorA", Color{0, 0, 255, 255});
        geometry->setColor("colorB", Color{0, 0, 255, 255});
        Node *output = graph.addNode("out.video", 900, 200);
        graph.connect(pass->id, 0, geometry->id, 0);
        graph.connect(geometry->id, 0, output->id, 0);
    } else if (stage == 12) {
        // solid red -> postfx -> output
        graph.clear();
        Node *pass = graph.addNode("shader.pass", 400, 200);
        pass->setInt("preset", 1);
        pass->setColor("colorA", Color{255, 40, 40, 255});
        Node *post = graph.addNode("fx.postfx", 650, 200);
        Node *output = graph.addNode("out.video", 900, 200);
        graph.connect(pass->id, 0, post->id, 0);
        graph.connect(post->id, 0, output->id, 0);
    } else if (stage == 13) {
        for (auto &node : project.graph.nodes) {
            if (node.kind == "shader.pass") node.setInt("preset", 1);  // solid
        }
    } else if (stage == 14) {
        for (auto &node : project.graph.nodes) {
            if (node.kind == "fx.postfx") {
                node.setFloat("bloom", 0.0f);
                node.setFloat("chromatic", 0.0f);
                node.setFloat("vignette", 0.0f);
                node.setFloat("grain", 0.0f);
            }
        }
    } else if (stage == 15) {
        // default project with geometry bypassed entirely: post <- pass
        int passId = 0;
        for (const auto &node : project.graph.nodes) {
            if (node.kind == "shader.pass") passId = node.id;
        }
        for (const auto &node : project.graph.nodes) {
            if (node.kind == "fx.postfx") {
                project.graph.disconnectInput(node.id, 0);
                project.graph.connect(passId, 0, node.id, 0);
            }
        }
    } else if (stage == 6) {
        // default project, but with the shader pass feedback disabled
        for (auto &node : project.graph.nodes) {
            if (node.kind == "shader.pass") node.setBool("useFeedback", false);
        }
    } else if (stage == 7) {
        // default project, but the geometry block is bypassed
        const int passId = [&]() {
            for (const auto &node : project.graph.nodes) {
                if (node.kind == "shader.pass") return node.id;
            }
            return 0;
        }();
        for (auto &node : project.graph.nodes) {
            if (node.kind != "geom.primitives") continue;
            const Link *link = project.graph.findInputLink(node.id, 0);
            if (!link || link->fromNode != passId) continue;
            const int geometryId = node.id;
            for (auto &target : project.graph.nodes) {
                if (target.kind != "fx.postfx") continue;
                project.graph.disconnectInput(target.id, 0);
                project.graph.connect(passId, 0, target.id, 0);
            }
            (void)geometryId;
        }
    }

    if (stage == 20) {
        // exercises the .glsl file path and the shared preamble
        graph.clear();
        Node *audioNode = graph.addNode("src.audio", 40, 200);
        Node *analyzer = graph.addNode("dsp.analyze", 240, 200);
        Node *pass = graph.addNode("shader.pass", 480, 120);
        pass->setText("shader", "assets/shaders/spiral_tunnel.glsl");
        pass->setBool("useFeedback", true);
        pass->setFloat("feedback", 0.6f);
        pass->setColor("colorA", Color{60, 120, 255, 255});
        pass->setColor("colorB", Color{255, 80, 180, 255});
        Node *output = graph.addNode("out.video", 760, 160);
        graph.connect(audioNode->id, 0, analyzer->id, 0);
        graph.connect(analyzer->id, 3, pass->id, 1);
        graph.connect(pass->id, 0, output->id, 0);
    }

    if (stage < 3 || stage == 4 || stage == 5) {
        graph.clear();
        Node *audioNode = graph.addNode("src.audio", 40, 200);
        Node *analyzer = graph.addNode("dsp.analyze", 240, 200);
        Node *pass = graph.addNode("shader.pass", 480, 120);
        pass->setInt("preset", 3);
        pass->setColor("colorA", Color{60, 120, 255, 255});
        pass->setColor("colorB", Color{255, 60, 170, 255});
        graph.connect(audioNode->id, 0, analyzer->id, 0);
        graph.connect(analyzer->id, 3, pass->id, 1);
        graph.connect(analyzer->id, 4, pass->id, 2);
        Node *tail = pass;
        if (stage >= 1) {
            Node *geometry = graph.addNode("geom.primitives", 720, 260);
            geometry->setInt("shape", 2);
            geometry->setInt("count", 64);
            graph.connect(tail->id, 0, geometry->id, 0);
            tail = geometry;
        }
        if (stage >= 2 && stage != 4 && stage != 5) {
            Node *post = graph.addNode("fx.postfx", 960, 260);
            graph.connect(tail->id, 0, post->id, 0);
            tail = post;
        }
        if (stage == 4 || stage == 5) {
            Node *chain = graph.addNode("shader.pass", 960, 300);
            chain->setInt("preset", 0);  // passthrough builtin
            graph.connect(tail->id, 0, chain->id, 0);
            tail = chain;
        }
        Node *output = graph.addNode("out.video", 1200, 260);
        graph.connect(tail->id, 0, output->id, 0);
    }

    EvalContext ctx;
    ctx.width = width;
    ctx.height = height;
    ctx.fps = 30.0f;
    ctx.duration = clip.duration();
    ctx.time = time;
    ctx.frame = static_cast<int>(time * 30.0);
    ctx.audioTime = time;
    ctx.audio = clip.buffer();
    ctx.analysis = analysis;

    ImageBufferPtr image = renderer.renderFrame(graph, ctx, &error);
    if (!image) {
        std::printf("render failed: %s\n", error.c_str());
        return 1;
    }

    FrameReadback readback;
    readback.capture(image->texture);
    readback.capture(image->texture);  // second capture makes the first fetchable
    const unsigned char *pixels = readback.fetch();
    if (!pixels) {
        std::printf("readback failed\n");
        return 1;
    }

    double sum[3] = {0, 0, 0};
    for (int i = 0; i < width * height; ++i) {
        for (int c = 0; c < 3; ++c) sum[c] += pixels[i * 4 + c];
    }
    const double count = static_cast<double>(width * height);
    std::printf("stage %d  time %.2f  mean rgb = (%.1f, %.1f, %.1f)\n", stage, time,
                sum[0] / count, sum[1] / count, sum[2] / count);

    Image out{};
    out.data = const_cast<unsigned char *>(pixels);
    out.width = width;
    out.height = height;
    out.mipmaps = 1;
    out.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    const bool exported = ExportImage(out, output.c_str());
    std::printf("wrote %s : %s\n", output.c_str(), exported ? "ok" : "failed");
    readback.release();
    readback.shutdown();

    for (const auto &node : project.graph.nodes) {
        std::printf("  %-16s %-22s %6.2f ms  %s\n", node.kind.c_str(),
                    node.displayTitle().c_str(), node.lastEvalMs, node.status.c_str());
    }

    // Dump every intermediate image so a broken stage can be spotted at once.
    for (auto &node : project.graph.nodes) {
        if (node.outputs.empty() || !node.outputs[0].image) continue;
        const ImageBufferPtr &buffer = node.outputs[0].image;
        if (!buffer->valid()) continue;
        FrameReadback nodeReadback;
        nodeReadback.capture(buffer->texture);
        nodeReadback.capture(buffer->texture);
        const unsigned char *nodePixels = nodeReadback.fetch();
        if (!nodePixels) continue;
        double nodeSum[3] = {0, 0, 0};
        const int nodeCount = buffer->width * buffer->height;
        for (int i = 0; i < nodeCount; ++i) {
            for (int c = 0; c < 3; ++c) nodeSum[c] += nodePixels[i * 4 + c];
        }
        std::printf("  node %-16s %4dx%-4d mean=(%.1f,%.1f,%.1f)\n", node.kind.c_str(),
                    buffer->width, buffer->height, nodeSum[0] / nodeCount,
                    nodeSum[1] / nodeCount, nodeSum[2] / nodeCount);
        char name[128];
        std::snprintf(name, sizeof(name), "node_%d_%s.png", node.id, node.kind.c_str());
        Image nodeImage{};
        nodeImage.data = const_cast<unsigned char *>(nodePixels);
        nodeImage.width = buffer->width;
        nodeImage.height = buffer->height;
        nodeImage.mipmaps = 1;
        nodeImage.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
        ExportImage(nodeImage, name);
        nodeReadback.release();
        nodeReadback.shutdown();
    }

    renderer.shutdown();
    CloseAudioDevice();
    CloseWindow();
    std::printf("done\n");
    return 0;
}
