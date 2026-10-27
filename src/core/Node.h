// Block ("node") model: ports, parameters, evaluation signature.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
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
    // True while an ADC-driven Scalar region is evaluated once per audio sample
    // instead of once per video frame. A downstream DAC is optional.
    bool audioRate = false;
    int audioSampleRate = 48000;
    // Incremented once per Graph::evaluate pass. Audio-rate nodes use it to
    // cache parameter lookups for the whole sample loop while still seeing an
    // edit made on the next displayed frame.
    std::uint64_t evaluation = 0;

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
    // True when the most recent graph pass evaluated this node inside the
    // compiled audio-rate region. Runtime only; never serialised.
    bool runsAudioRate = false;

    const NodeDef *def = nullptr;
    std::vector<Param> params;
    // Blocks whose ports follow their configuration (a Shader block exposes the
    // uniforms its .glsl file uses) resolve them here; when empty, the ports of
    // the definition apply.
    std::vector<PortDesc> dynamicInputs;
    // Blocks whose outputs follow their configuration (ADC channel count)
    // resolve them here; when empty, the ports of the definition apply.
    std::vector<PortDesc> dynamicOutputs;

    const std::vector<PortDesc> &inputPorts() const {
        static const std::vector<PortDesc> none;
        if (!dynamicInputs.empty()) return dynamicInputs;
        return def ? def->inputs : none;
    }
    const std::vector<PortDesc> &outputPorts() const {
        static const std::vector<PortDesc> none;
        if (!dynamicOutputs.empty()) return dynamicOutputs;
        return def ? def->outputs : none;
    }
    void setInputPorts(std::vector<PortDesc> ports) { dynamicInputs = std::move(ports); }
    void clearInputPorts() { dynamicInputs.clear(); }
    void setOutputPorts(std::vector<PortDesc> ports) { dynamicOutputs = std::move(ports); }
    void clearOutputPorts() { dynamicOutputs.clear(); }

    // Results of the most recent evaluation (valid for the current frame only).
    std::vector<Value> outputs;
    double lastEvalMs = 0.0;
    std::string status;
    // Scratch values kept between frames by stateful blocks (envelopes, LFOs,
    // particles). Never serialised.
    std::unordered_map<std::string, double> runtimeState;
    // Input values reused by the evaluator so audio-rate regions do not
    // allocate per sample. Never serialised.
    std::vector<Value> inputScratch;
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
    // Write cursor used by the audio-rate scheduler; -1 when the window is
    // already cached and must not be touched. Never serialised.
    long long audioRenderWrite = -1;

    // Rolling sample buffers kept for the in-block visualisations. Not
    // serialised; the editor reads them directly while drawing the graph.
    std::vector<float> historyA;
    std::vector<float> historyB;
    int historyCount = 0;  // samples pushed so far, used to fade in
    // O(1) ring used by the Ringbuffer block at audio rate. `ringHead` is the
    // oldest sample while ringCount > 0. Never serialised.
    std::vector<float> ringValues;
    int ringHead = 0;
    int ringCount = 0;
    double ringSum = 0.0;
    // Typed state and per-evaluation constants for the Ringbuffer's audio-rate
    // hot path. runtimeState is still updated on the final sample for the UI.
    std::uint64_t ringEvalSerial = 0;
    int ringCapacity = 0;
    float ringBaseSpeed = 0.5f;
    float ringPhaseOffset = 0.0f;
    double ringPhase = 0.0;
    int ringVisualFrame = -1;
    bool ringRateResolved = false;
    bool ringAtAudioRate = false;
    int ringSampleRate = 0;
    // Signal Filter coefficients' two state words. Keeping these out of the
    // string-keyed runtime map matters when the filter runs per audio sample.
    std::uint64_t filterEvalSerial = 0;
    int filterMode = 0;
    float filterBaseCutoff = 4.0f;
    float filterBaseResonance = 0.707f;
    double filterZ1 = 0.0;
    double filterZ2 = 0.0;
    // Meter ballistics and display history use the same typed-state treatment.
    std::uint64_t meterEvalSerial = 0;
    int meterMode = 0;
    float meterRelease = 0.55f;
    double meterAverage = 0.0;
    double meterPeak = 0.0;
    int meterVisualFrame = -1;
    // Rolling mono history of the audio a Spectrum block draws. A live
    // analysis (a processed chain) only carries one video frame of source at a
    // time, so the block accumulates the last ~0.12 s to fill its waveform
    // window; `waveHead` is the oldest sample while waveCount > 0 and the
    // newest sample sits just before `waveEndFrame` (clip frames). Never
    // serialised.
    std::vector<float> waveHistory;
    int waveHead = 0;
    int waveCount = 0;
    long long waveEndFrame = -1;

    // Pushes a sample into one of the rolling buffers (oldest first once full).
    static void pushHistory(std::vector<float> &buffer, float value, int capacity);
    // Reads a sample where 0 is the oldest and 1 the newest.
    static float historyAt(const std::vector<float> &buffer, int capacity, int count, float position);
    // Appends mono samples covering clip frames [startFrame, startFrame+frames)
    // to the Spectrum wave history, resetting it when the incoming window does
    // not continue what is already there (a seek or a rewired source).
    static void pushWaveHistory(Node &node, int sampleRate, const float *samples, int channels,
                                long long frames, long long startFrame);
    // Interpolated sample at a clip frame, 0 outside the ring.
    float waveHistoryAt(double frame) const;

    const Param *find(std::string_view key) const;
    Param *find(std::string_view key);

    float pfloat(std::string_view key, float fallback = 0.0f) const;
    int pint(std::string_view key, int fallback = 0) const;
    bool pbool(std::string_view key, bool fallback = false) const;
    Color pcolor(std::string_view key) const;
    const std::string &pstr(std::string_view key) const;
    float curveAt(std::string_view key, double normalizedTime) const;

    void setFloat(std::string_view key, float value);
    void setInt(std::string_view key, int value);
    void setBool(std::string_view key, bool value);
    void setText(std::string_view key, const std::string &value);
    void setColor(std::string_view key, Color value);

    // A block publishes the value it actually used after modulation
    // (`value.<key>` in runtimeState) so the inspector slider and the in-block
    // previews can follow an LFO instead of only showing the base parameter.
    // The audio-rate region clears this for every sample but the last of a frame
    // (see compileRegion in Graph.cpp): a string-keyed map write per sample is
    // measurable, and only the frame's final value is ever shown.
    bool publishReadouts = true;
    void publishEffective(std::string_view key, float value) {
        // An audio-rate region calls a block once per sample; the readouts it
        // publishes are only there for the editor, which looks at the frame's
        // last sample, so the loop turns this off for the samples before it.
        if (!publishReadouts) return;
        std::string stateKey = "value.";
        stateKey.append(key);
        runtimeState[std::move(stateKey)] = static_cast<double>(value);
    }
    bool effectiveParam(std::string_view key, float *value) const {
        std::string stateKey = "value.";
        stateKey.append(key);
        const auto it = runtimeState.find(stateKey);
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
