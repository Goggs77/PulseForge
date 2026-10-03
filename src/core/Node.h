// Block ("node") model: ports, parameters, evaluation signature.
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "core/Port.h"

namespace pf {

class Renderer;
class ShaderLibrary;

// Per-frame state handed to every block while the graph is evaluated.
struct EvalContext {
    double time = 0.0;      // seconds into the rendered video
    double duration = 1.0;  // total seconds
    int frame = 0;
    float fps = 60.0f;
    int width = 1920;
    int height = 1080;
    bool offline = false;   // true while exporting
    double audioTime = 0.0; // position inside the audio clip

    // Frame-level analysis, filled in once per frame by the renderer.
    float bass = 0.0f;
    float mid = 0.0f;
    float treble = 0.0f;
    float level = 0.0f;
    float onset = 0.0f;
    float rms = 0.0f;
    float beat = 0.0f;

    Renderer *renderer = nullptr;
    ShaderLibrary *shaders = nullptr;
    AnalysisPtr analysis;
    AudioPtr audio;
    std::string projectDir;
    std::string error;
};

struct PortDesc {
    std::string name;
    PortType type = PortType::None;
    std::string hint;
};

class Node;

using NodeEvalFn = void (*)(Node &node, EvalContext &ctx, const std::vector<Value> &in,
                            std::vector<Value> &out);

struct NodeDef {
    std::string kind;
    std::string category;
    std::string label;
    std::string description;
    std::vector<PortDesc> inputs;
    std::vector<PortDesc> outputs;
    std::vector<Param> params;
    NodeEvalFn evaluate = nullptr;
    bool isSink = false;
};

class Node {
public:
    int id = 0;
    std::string kind;
    std::string title;
    float x = 0.0f;
    float y = 0.0f;
    bool enabled = true;

    const NodeDef *def = nullptr;
    std::vector<Param> params;
    // Blocks whose ports follow their configuration (a Shader block exposes the
    // uniforms its .glsl file uses) resolve them here; when empty, the ports of
    // the definition apply.
    std::vector<PortDesc> dynamicInputs;

    const std::vector<PortDesc> &inputPorts() const {
        static const std::vector<PortDesc> none;
        if (!dynamicInputs.empty()) return dynamicInputs;
        return def ? def->inputs : none;
    }
    const std::vector<PortDesc> &outputPorts() const {
        static const std::vector<PortDesc> none;
        return def ? def->outputs : none;
    }
    void setInputPorts(std::vector<PortDesc> ports) { dynamicInputs = std::move(ports); }
    void clearInputPorts() { dynamicInputs.clear(); }

    // Results of the most recent evaluation (valid for the current frame only).
    std::vector<Value> outputs;
    double lastEvalMs = 0.0;
    std::string status;
    // Scratch values kept between frames by stateful blocks (envelopes, LFOs,
    // particles). Never serialised.
    std::unordered_map<std::string, double> runtimeState;
    // Live analysis of a Spectrum Analyzer's own Audio input when it is not the
    // project's decoded clip (processed audio). Never serialised.
    AnalysisPtr runtimeAnalysis;
    std::string runtimeAnalysisKey;

    // Audio rendered by a block that transforms the Audio stream (Dynamics).
    // `audioRenderOutput` starts at `audioRenderStart` (a frame index inside the
    // source clip) and grows as the graph is evaluated forward; the exporter
    // uses it to mux the processed stream. Never serialised.
    std::shared_ptr<AudioBuffer> audioRenderOutput;
    long long audioRenderStart = -1;
    long long audioRenderFrames = 0;
    std::string audioRenderKey;

    // Rolling sample buffers kept for the in-block visualisations. Not
    // serialised; the editor reads them directly while drawing the graph.
    std::vector<float> historyA;
    std::vector<float> historyB;
    int historyCount = 0;  // samples pushed so far, used to fade in

    // Pushes a sample into one of the rolling buffers (oldest first once full).
    static void pushHistory(std::vector<float> &buffer, float value, int capacity);
    // Reads a sample where 0 is the oldest and 1 the newest.
    static float historyAt(const std::vector<float> &buffer, int capacity, int count, float position);

    const Param *find(const std::string &key) const;
    Param *find(const std::string &key);

    float pfloat(const std::string &key, float fallback = 0.0f) const;
    int pint(const std::string &key, int fallback = 0) const;
    bool pbool(const std::string &key, bool fallback = false) const;
    Color pcolor(const std::string &key) const;
    const std::string &pstr(const std::string &key) const;
    float curveAt(const std::string &key, double normalizedTime) const;

    void setFloat(const std::string &key, float value);
    void setInt(const std::string &key, int value);
    void setBool(const std::string &key, bool value);
    void setText(const std::string &key, const std::string &value);
    void setColor(const std::string &key, Color value);

    // A block publishes the value it actually used after modulation
    // (`value.<key>` in runtimeState) so the inspector slider and the in-block
    // previews can follow an LFO instead of only showing the base parameter.
    void publishEffective(const std::string &key, float value) {
        runtimeState["value." + key] = static_cast<double>(value);
    }
    bool effectiveParam(const std::string &key, float *value) const {
        const auto it = runtimeState.find("value." + key);
        if (it == runtimeState.end()) return false;
        if (value) *value = static_cast<float>(it->second);
        return true;
    }

    // Fills in any missing parameters from the definition; keeps values of
    // matching keys so saved projects stay compatible when a block gains one.
    void ensureParams(const NodeDef &definition);

    std::string displayTitle() const { return title.empty() ? (def ? def->label : kind) : title; }
};

}  // namespace pf
