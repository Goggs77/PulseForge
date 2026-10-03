// Built-in blocks: sources, DSP/analysis, timing, modulation, render and output.
#include "core/Registry.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "dsp/Analysis.h"
#include "dsp/Dynamics.h"
#include "render/Geometry.h"
#include "render/Palette.h"
#include "render/Renderer.h"
#include "render/ShaderLibrary.h"

namespace pf {

namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Samples kept for the Frequency Band block's live input/output bar.
constexpr int kBandHistory = 48;
// Samples kept for the VU / Digital Meter's value-time diagram (4 s at 60 fps).
constexpr int kMeterHistory = 240;

// Evaluation rate of the current pass: the audio sample rate inside an
// ADC -> DAC region, otherwise the project frame rate.
float rateOf(const EvalContext &ctx) {
    if (ctx.audioRate) return static_cast<float>(std::max(1, ctx.audioSampleRate));
    return ctx.fps > 1.0f ? ctx.fps : 60.0f;
}

float dtOf(const EvalContext &ctx) { return 1.0f / rateOf(ctx); }

float logFrequencyPosition(float hz, int sampleRate) {
    const float nyquist = std::max(1000.0f, static_cast<float>(sampleRate) * 0.5f);
    const float lo = 20.0f;
    const float clamped = std::clamp(hz, lo, nyquist);
    return std::log(clamped / lo) / std::log(nyquist / lo);
}

// One-pole attack/release follower with per-node state.
float follow(Node &node, const char *key, float target, float attack, float release, float dt) {
    const float current = static_cast<float>(node.runtimeState[key]);
    const float coefficient = target > current ? std::clamp(attack, 0.001f, 1.0f)
                                               : std::clamp(release, 0.001f, 1.0f);
    // coefficient is interpreted as the fraction covered per 1/60 s.
    const float k = 1.0f - std::pow(1.0f - coefficient, std::max(dt, 1e-4f) * 60.0f);
    const float next = current + (target - current) * k;
    node.runtimeState[key] = next;
    return next;
}

const AnalysisData *analysisFrom(const std::vector<Value> &in, const EvalContext &ctx, size_t port) {
    // A connected Analysis port is authoritative even when it carries no data;
    // only an unconnected port falls back to the project's decoded analysis.
    if (port < in.size() && in[port].type == PortType::Analysis) return in[port].analysis.get();
    return ctx.analysis.get();
}

float scalarFrom(const std::vector<Value> &in, size_t port) {
    if (port < in.size() && in[port].type == PortType::Scalar) return in[port].scalar;
    return 0.0f;
}

Texture2D textureFrom(const std::vector<Value> &in, size_t port) {
    if (port < in.size() && in[port].image && in[port].image->valid()) return in[port].image->texture.texture;
    return Texture2D{};
}

// ---------------------------------------------------------------------------
// Sources
// ---------------------------------------------------------------------------

void evalAudioSource(Node &node, EvalContext &ctx, const std::vector<Value> &in,
                     std::vector<Value> &out) {
    (void)node;
    (void)in;
    out[0].type = PortType::Audio;
    out[0].audio = ctx.audio;
}

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------

void evalClock(Node &node, EvalContext &ctx, const std::vector<Value> &in, std::vector<Value> &out) {
    (void)in;
    const float speed = node.pfloat("speed", 1.0f);
    const float offset = node.pfloat("offset", 0.0f);
    const float bpm = std::max(1.0f, node.pfloat("bpm", 120.0f));
    const double time = ctx.time * speed + offset;
    out[0] = Value::makeScalar(static_cast<float>(time));
    out[1] = Value::makeScalar(static_cast<float>(ctx.frame));
    out[2] = Value::makeScalar(ctx.duration > 0.0
                                   ? static_cast<float>(std::clamp(time / ctx.duration, 0.0, 1.0))
                                   : 0.0f);
    const double beats = time * bpm / 60.0;
    out[3] = Value::makeScalar(static_cast<float>(beats - std::floor(beats)));
}

void evalPulse(Node &node, EvalContext &ctx, const std::vector<Value> &in, std::vector<Value> &out) {
    // Modulation ports are appended after the block's own inputs, so adding them
    // never renumbers the links of an existing project.
    const bool bpmConnected = in.size() > 0 && in[0].type == PortType::Scalar;
    const bool decayConnected = in.size() > 1 && in[1].type == PortType::Scalar;
    const bool offsetConnected = in.size() > 2 && in[2].type == PortType::Scalar;
    float bpm = node.pfloat("bpm", 120.0f);
    if (bpmConnected) bpm *= std::pow(2.0f, std::clamp(in[0].scalar, -4.0f, 4.0f));
    bpm = std::clamp(bpm, 20.0f, 300.0f);
    const int division = node.pint("division", 2);
    static const float divisors[] = {1.0f, 0.5f, 0.25f, 0.125f, 0.0625f};
    const float beatLength = 60.0f / bpm * divisors[std::clamp(division, 0, 4)];
    float decay = node.pfloat("decay", 0.5f);
    if (decayConnected) decay *= std::max(0.0f, 1.0f + in[1].scalar);
    decay = std::clamp(decay, 0.02f, 2.0f);
    float offset = node.pfloat("offset", 0.0f);
    if (offsetConnected) offset += in[2].scalar * 0.25f;  // quarter of a beat nudge
    node.publishEffective("bpm", bpm);
    node.publishEffective("decay", decay);
    node.publishEffective("offset", offset);
    const double phase = (ctx.time + offset) / std::max(1e-4f, beatLength);
    const float fract = static_cast<float>(phase - std::floor(phase));
    out[0] = Value::makeScalar(std::exp(-fract * decay * 9.0f));
}

// ---------------------------------------------------------------------------
// Analysis / DSP
// ---------------------------------------------------------------------------

void evalAnalyzer(Node &node, EvalContext &ctx, const std::vector<Value> &in, std::vector<Value> &out) {
    const float gain = node.pfloat("gain", 1.0f);
    const float gate = node.pfloat("gate", 0.0f);
    out[0].type = PortType::Analysis;
    const AudioPtr audio =
        (!in.empty() && in[0].type == PortType::Audio) ? in[0].audio : AudioPtr();
    if (!audio) {
        // Fully input driven: with nothing wired here there is nothing to
        // analyse, even when the project has imported media.
        for (size_t i = 1; i < out.size(); ++i) out[i] = Value::makeScalar(0.0f);
        node.runtimeAnalysis.reset();
        node.runtimeAnalysisKey.clear();
        node.status = "connect an Audio input";
        return;
    }

    AnalysisPtr analysis;
    if (ctx.analysis && audio == ctx.audio) {
        // The project's decoded clip already has a whole-file analysis.
        analysis = ctx.analysis;
    } else {
        // Processed or foreign audio has no whole-file analysis, so one FFT
        // window is measured for the current video frame.
        const float fs = rateOf(ctx);
        double windowEnd = ctx.audioTime + 1.0 / fs;
        if (!ctx.offline) {
            // Follow the video frame the preview is showing, not the continuous
            // display playhead.
            const double quantized = static_cast<double>(ctx.frame) / fs;
            windowEnd = ctx.audioTime + (quantized - ctx.time) + 1.0 / fs;
        }
        char key[160];
        std::snprintf(key, sizeof(key), "%p|%lld|%d", static_cast<const void *>(audio.get()),
                      std::llround(windowEnd * std::max(1, audio->sampleRate)),
                      audio->sampleRate);
        if (!node.runtimeAnalysis || node.runtimeAnalysisKey != key) {
            // Include the video frame's freshly generated samples: the region
            // ran before this block in the same evaluation.
            node.runtimeAnalysis =
                analyzeWindow(*audio, windowEnd, AnalysisSettings{}, audio);
            node.runtimeAnalysisKey = key;
        }
        analysis = node.runtimeAnalysis;
    }
    out[0].analysis = analysis;
    node.status.clear();
    if (!analysis || analysis->frames.empty()) {
        for (size_t i = 1; i < out.size(); ++i) out[i] = Value::makeScalar(0.0f);
        return;
    }

    auto shape = [&](float value) {
        value *= gain;
        return value < gate ? 0.0f : std::clamp(value, 0.0f, 1.0f);
    };
    const float *row = analysis->spectrumRow(ctx.audioTime);
    float bass = 0.0f, mid = 0.0f, treble = 0.0f;
    if (row) {
        const int bins = analysis->spectrumBins;
        const int bassEnd = std::max(1, bins / 8);
        const int midEnd = std::max(bassEnd + 1, bins / 2);
        auto average = [&](int from, int to) {
            float sum = 0.0f;
            for (int i = from; i < to; ++i) sum += row[i];
            return to > from ? sum / static_cast<float>(to - from) : 0.0f;
        };
        bass = average(0, bassEnd);
        mid = average(bassEnd, midEnd);
        treble = average(midEnd, bins);
    }
    out[1] = Value::makeScalar(shape(analysis->valueAt(ctx.audioTime, 1)));
    out[2] = Value::makeScalar(shape(analysis->valueAt(ctx.audioTime, 3)));
    out[3] = Value::makeScalar(shape(bass));
    out[4] = Value::makeScalar(shape(mid));
    out[5] = Value::makeScalar(shape(treble));
}

void evalBand(Node &node, EvalContext &ctx, const std::vector<Value> &in, std::vector<Value> &out) {
    const AnalysisData *analysis = analysisFrom(in, ctx, 0);
    const float gain = node.pfloat("gain", 1.0f);
    const float curve = std::max(0.05f, node.pfloat("curve", 1.0f));
    const float gate = node.pfloat("gate", 0.0f);
    if (!analysis || analysis->spectrumBins <= 0) {
        out[0] = Value::makeScalar(0.0f);
        return;
    }
    const float nyquist = std::max(1000.0f, static_cast<float>(analysis->sampleRate) * 0.5f);
    const float lo = logFrequencyPosition(node.pfloat("lowHz", 40.0f), analysis->sampleRate);
    const float hi = logFrequencyPosition(std::max(node.pfloat("highHz", 250.0f),
                                                   node.pfloat("lowHz", 40.0f) + 1.0f),
                                          analysis->sampleRate);
    const int bins = analysis->spectrumBins;
    const int from = std::clamp(static_cast<int>(lo * bins), 0, bins - 1);
    const int to = std::clamp(static_cast<int>(hi * bins) + 1, from + 1, bins);
    const float *row = analysis->spectrumRow(ctx.audioTime);
    float value = 0.0f;
    if (row) {
        const int mode = node.pint("mode", 0);
        float sum = 0.0f, peak = 0.0f;
        for (int i = from; i < to; ++i) {
            sum += row[i];
            peak = std::max(peak, row[i]);
        }
        switch (mode) {
            case 1: value = peak; break;
            case 2: value = sum; break;
            default: value = sum / static_cast<float>(std::max(1, to - from)); break;
        }
    }
    value = std::pow(std::clamp(value * gain, 0.0f, 1.0f), curve);
    if (value < gate) value = 0.0f;
    // Raw and shaped values are kept for the block's live bar.
    const float raw = value;
    const float smoothed = follow(node, "env", value, node.pfloat("attack", 0.5f),
                                  node.pfloat("release", 0.12f), dtOf(ctx));
    node.runtimeState["in"] = raw;
    node.runtimeState["out"] = smoothed;
    Node::pushHistory(node.historyA, raw, kBandHistory);
    Node::pushHistory(node.historyB, smoothed, kBandHistory);
    ++node.historyCount;
    out[0] = Value::makeScalar(smoothed);
    (void)nyquist;
}

void evalEnvelope(Node &node, EvalContext &ctx, const std::vector<Value> &in, std::vector<Value> &out) {
    float value = std::clamp(scalarFrom(in, 0), 0.0f, 4.0f);
    const float gate = node.pfloat("threshold", 0.0f);
    if (value < gate) value = 0.0f;
    const float smoothed = follow(node, "env", value, node.pfloat("attack", 0.4f),
                                  node.pfloat("release", 0.1f), dtOf(ctx));
    float result = smoothed * node.pfloat("gain", 1.0f) + node.pfloat("offset", 0.0f);
    result = std::max(result, node.pfloat("floor", 0.0f));
    out[0] = Value::makeScalar(result);
}

void evalRemap(Node &node, EvalContext &ctx, const std::vector<Value> &in, std::vector<Value> &out) {
    (void)ctx;
    const float value = scalarFrom(in, 0);
    float t = normalize01(value, node.pfloat("inMin", 0.0f), node.pfloat("inMax", 1.0f));
    if (node.pbool("smoothstep", false)) t = t * t * (3.0f - 2.0f * t);
    const float power = std::max(0.05f, node.pfloat("power", 1.0f));
    if (std::fabs(power - 1.0f) > 1e-3f) t = std::pow(t, power);
    float result = lerp(node.pfloat("outMin", 0.0f), node.pfloat("outMax", 1.0f), t);
    if (node.pbool("clamp", true)) {
        const float lo = std::min(node.pfloat("outMin", 0.0f), node.pfloat("outMax", 1.0f));
        const float hi = std::max(node.pfloat("outMin", 0.0f), node.pfloat("outMax", 1.0f));
        result = std::clamp(result, lo, hi);
    }
    out[0] = Value::makeScalar(result);
}

// Single-band dynamics. The Audio stream is processed one video frame at a
// time; the envelope state and the partially rendered clip live on the node, so
// a sequential pass (playback, export) builds the whole processed buffer while a
// seek restarts from the requested window. Inputs 1..6 are the appended
// modulation ports (Pre-gain, Threshold, Ratio, Attack, Release, Post-gain).
void evalDynamics(Node &node, EvalContext &ctx, const std::vector<Value> &in,
                  std::vector<Value> &out) {
    const AudioBuffer *source = nullptr;
    if (!in.empty() && in[0].audio) source = in[0].audio.get();
    else if (ctx.audio) source = ctx.audio.get();

    DynamicsSettings settings;
    settings.expand = node.pint("mode", 0) == 1;
    settings.preGainDb = node.pfloat("preGain", 0.0f);
    settings.thresholdDb = node.pfloat("threshold", -18.0f);
    settings.ratio = node.pfloat("ratio", 4.0f);
    settings.attackMs = node.pfloat("attack", 10.0f);
    settings.releaseMs = node.pfloat("release", 150.0f);
    settings.postGainDb = node.pfloat("postGain", 0.0f);
    settings.limiter = node.pbool("limiter", true);
    settings.softClip = node.pint("limiterMode", 1) == 1;
    settings.limiterAttackMs = node.pfloat("limiterAttack", 1.0f);
    settings.limiterReleaseMs = node.pfloat("limiterRelease", 80.0f);

    // Levels scale by (1 + input): the block's dB value moves by the gain the
    // input asks for. Thresholds and times keep their own conventions.
    auto levelInputDb = [&](size_t port) {
        if (in.size() <= port || in[port].type != PortType::Scalar) return 0.0f;
        const float factor = std::clamp(1.0f + in[port].scalar, 0.01f, 64.0f);
        return linearToDb(factor);
    };
    auto rateInput = [&](size_t port) {
        if (in.size() <= port || in[port].type != PortType::Scalar) return 1.0f;
        return std::pow(2.0f, std::clamp(in[port].scalar, -8.0f, 8.0f));
    };
    settings.preGainDb += levelInputDb(1);
    if (in.size() > 2 && in[2].type == PortType::Scalar) {
        settings.thresholdDb += in[2].scalar * 24.0f;  // full-scale modulation is +/-24 dB
    }
    if (in.size() > 3 && in[3].type == PortType::Scalar) {
        settings.ratio *= std::clamp(1.0f + in[3].scalar, 0.05f, 32.0f);
    }
    settings.attackMs *= rateInput(4);
    settings.releaseMs *= rateInput(5);
    settings.postGainDb += levelInputDb(6);
    settings.thresholdDb = std::clamp(settings.thresholdDb, -96.0f, 24.0f);
    settings.ratio = std::clamp(settings.ratio, 1.0f, 100.0f);
    settings.attackMs = std::clamp(settings.attackMs, 0.05f, 2000.0f);
    settings.releaseMs = std::clamp(settings.releaseMs, 1.0f, 8000.0f);

    // The inspector and the in-block readouts follow the values actually used.
    node.publishEffective("preGain", settings.preGainDb);
    node.publishEffective("threshold", settings.thresholdDb);
    node.publishEffective("ratio", settings.ratio);
    node.publishEffective("attack", settings.attackMs);
    node.publishEffective("release", settings.releaseMs);
    node.publishEffective("postGain", settings.postGainDb);

    out[0].type = PortType::Audio;
    if (!source) return;
    out[0].audio = node.audioRenderOutput;  // may still be empty until the window is rendered
    const int channels = std::max(1, source->channels);
    const int sampleRate = std::max(8000, source->sampleRate);
    const long long frameCount = source->frameCount;
    if (frameCount <= 0 || source->samples.empty()) return;

    // Cache key: the input clip plus the *base* parameters. Modulated values are
    // deliberately excluded so an LFO moving the threshold keeps appending to
    // the same buffer instead of restarting the render every frame.
    char key[320];
    std::snprintf(key, sizeof(key), "%p|%d|%d|%lld|%d|%.5f|%.5f|%.5f|%.5f|%.5f|%.5f|%d|%d|%.5f|%.5f",
                  static_cast<const void *>(source), channels, sampleRate, frameCount,
                  node.pint("mode", 0), node.pfloat("preGain", 0.0f),
                  node.pfloat("threshold", -18.0f), node.pfloat("ratio", 4.0f),
                  node.pfloat("attack", 10.0f), node.pfloat("release", 150.0f),
                  node.pfloat("postGain", 0.0f), node.pbool("limiter", true) ? 1 : 0,
                  node.pint("limiterMode", 1), node.pfloat("limiterAttack", 1.0f),
                  node.pfloat("limiterRelease", 80.0f));

    const float fps = ctx.fps > 1.0f ? ctx.fps : 60.0f;
    const long long startFrame =
        std::clamp<long long>(std::llround(ctx.audioTime * sampleRate), 0, frameCount);
    const long long endFrame = std::clamp<long long>(
        std::llround((ctx.audioTime + 1.0 / std::max(1.0f, fps)) * sampleRate), startFrame,
        frameCount);

    bool reusable = node.audioRenderOutput && node.audioRenderKey == key &&
                    startFrame >= node.audioRenderStart &&
                    endFrame <= node.audioRenderStart + node.audioRenderFrames;
    if (!reusable) {
        const bool append = node.audioRenderOutput && node.audioRenderKey == key &&
                            startFrame == node.audioRenderStart + node.audioRenderFrames;
        if (!append) {
            node.audioRenderOutput = std::make_shared<AudioBuffer>();
            node.audioRenderOutput->channels = channels;
            node.audioRenderOutput->sampleRate = sampleRate;
            node.audioRenderOutput->frameCount = 0;
            node.audioRenderOutput->startFrame = startFrame;
            node.audioRenderStart = startFrame;
            node.audioRenderFrames = 0;
            node.audioRenderKey = key;
            node.runtimeState.erase("dyn.detector");
            node.runtimeState.erase("dyn.gain");
            node.runtimeState.erase("dyn.limiterGain");
            out[0].audio = node.audioRenderOutput;
        }
        if (endFrame > startFrame) {
            AudioBuffer &rendered = *node.audioRenderOutput;
            const long long frames = endFrame - startFrame;
            const size_t base = rendered.samples.size();
            rendered.samples.resize(base + static_cast<size_t>(frames) * channels);

            DynamicsState state;
            const auto stored = [&](const char *name, float fallback) {
                const auto it = node.runtimeState.find(name);
                return it == node.runtimeState.end() ? fallback : static_cast<float>(it->second);
            };
            state.detector = stored("dyn.detector", 0.0f);
            state.gain = stored("dyn.gain", 1.0f);
            state.limiterGain = stored("dyn.limiterGain", 1.0f);
            processDynamicsBlock(source->samples.data() + static_cast<size_t>(startFrame) * channels,
                                 rendered.samples.data() + base, frames, channels, sampleRate, settings,
                                 state);
            node.runtimeState["dyn.detector"] = state.detector;
            node.runtimeState["dyn.gain"] = state.gain;
            node.runtimeState["dyn.limiterGain"] = state.limiterGain;
            node.audioRenderFrames += frames;
            rendered.frameCount = node.audioRenderFrames;
            out[0].audio = node.audioRenderOutput;
        }
    }

    // Dry and wet loudness for the 4:3 dBFS graph at the top of the block.
    const long long frames = endFrame - startFrame;
    const float dryDb = loudnessDb(*source, startFrame, frames);
    const long long wetOffset = startFrame - node.audioRenderStart;
    const bool wetAvailable = node.audioRenderOutput && wetOffset >= 0 &&
                              wetOffset + frames <= node.audioRenderFrames;
    const float wetDb =
        wetAvailable ? loudnessDb(*node.audioRenderOutput, wetOffset, frames) : dryDb;
    node.runtimeState["dryDb"] = dryDb;
    node.runtimeState["wetDb"] = wetDb;
    node.runtimeState["gainReductionDb"] = wetDb - dryDb;
    Node::pushHistory(node.historyA, dryDb, kMeterHistory);
    Node::pushHistory(node.historyB, wetDb, kMeterHistory);
    ++node.historyCount;
}

// ---------------------------------------------------------------------------
// Audio bridges
// ---------------------------------------------------------------------------

// Maps the current video-frame window onto an AudioBuffer. Rendered buffers
// (Dynamics, DAC) carry only the frames produced so far, so their startFrame is
// subtracted; a whole decoded clip starts at frame 0.
void audioWindowBounds(const EvalContext &ctx, const AudioBuffer &buffer, long long *start,
                       long long *end) {
    const int sampleRate = std::max(8000, buffer.sampleRate);
    const double dt = 1.0 / std::max(1.0f, ctx.fps > 1.0f ? ctx.fps : 60.0f);
    long long from = std::llround(ctx.audioTime * sampleRate) - buffer.startFrame;
    long long to = std::llround((ctx.audioTime + dt) * sampleRate) - buffer.startFrame;
    from = std::clamp(from, 0LL, buffer.frameCount);
    to = std::clamp(to, from, buffer.frameCount);
    if (to == from && from < buffer.frameCount) to = from + 1;
    *start = from;
    *end = to;
}

// ADC: Audio -> Scalar. The Scalar is a per-frame control value and it also
// carries the source stream, so a DAC further down can apply the value to the
// waveform sample-accurately. Unity (the default) makes ADC -> DAC a lossless
// round trip; RMS and Peak turn the block into a loudness follower that Math
// and Modulation blocks can process at frame rate.
void evalAdc(Node &node, EvalContext &ctx, const std::vector<Value> &in,
             std::vector<Value> &out) {
    AudioPtr carrier;
    if (!in.empty() && in[0].type == PortType::Audio) carrier = in[0].audio;
    const int channels = std::max(1, static_cast<int>(out.size()));
    if (!carrier || carrier->frameCount <= 0 || carrier->samples.empty()) {
        // Silence without an Audio input; never fall back to the imported clip.
        for (int c = 0; c < channels; ++c) {
            out[static_cast<size_t>(c)] = Value::makeScalar(0.0f);
        }
        node.status = "no audio";
        return;
    }
    for (int c = 0; c < channels; ++c) out[static_cast<size_t>(c)] = Value::makeScalar(1.0f);

    const int mode = node.pint("mode", 0);
    if (mode != 0) {
        long long start = 0;
        long long end = 0;
        audioWindowBounds(ctx, *carrier, &start, &end);
        const int sourceChannels = std::max(1, carrier->channels);
        const float *samples = carrier->samples.data();
        const double count = static_cast<double>(std::max<long long>(1, end - start));
        for (int c = 0; c < channels; ++c) {
            const int channel = std::min(c, sourceChannels - 1);
            double sumSquares = 0.0;
            float peak = 0.0f;
            for (long long frame = start; frame < end; ++frame) {
                const size_t index = static_cast<size_t>(frame) *
                                         static_cast<size_t>(sourceChannels) +
                                     static_cast<size_t>(channel);
                if (index >= carrier->samples.size()) break;
                float sample = samples[index];
                if (!std::isfinite(sample)) sample = 0.0f;
                sumSquares += static_cast<double>(sample) * sample;
                peak = std::max(peak, std::fabs(sample));
            }
            const float value =
                mode == 2 ? peak : static_cast<float>(std::sqrt(sumSquares / count));
            out[static_cast<size_t>(c)] = Value::makeScalar(value);
            node.runtimeState["value." + std::to_string(c)] = value;
        }
    }
    for (int c = 0; c < channels; ++c) out[static_cast<size_t>(c)].carrier = carrier;
}

// DAC: Scalar -> Audio. With a carrier the Scalar is a per-frame control value
// applied to the carried waveform sample by sample, so an untouched ADC -> DAC
// round trip is lossless. Without a carrier the Scalar itself becomes the
// sample value (DC/ramp synthesis). Offline passes accumulate the exported
// track; interactive playback keeps only the current window.
void evalDac(Node &node, EvalContext &ctx, const std::vector<Value> &in,
             std::vector<Value> &out) {
    out[0].type = PortType::Audio;
    const AudioPtr carrier =
        (!in.empty() && in[0].type == PortType::Scalar) ? in[0].carrier : nullptr;
    const AudioBuffer *format = carrier ? carrier.get() : ctx.audio.get();
    const int sampleRate = format ? std::clamp(format->sampleRate, 8000, 384000) : 48000;
    const int channels =
        std::clamp(static_cast<int>(node.inputPorts().size()), 1, 8);
    const long long formatStart = format ? format->startFrame : 0;
    const long long formatFrames = format ? format->frameCount : -1;
    const float fps = ctx.fps > 1.0f ? ctx.fps : 60.0f;
    long long localStart =
        std::max<long long>(0, std::llround(ctx.audioTime * sampleRate)) - formatStart;
    if (formatFrames >= 0) {
        localStart = std::clamp<long long>(localStart, 0, formatFrames);
    } else {
        localStart = std::max<long long>(0, localStart);
    }
    long long localEnd =
        localStart + std::max<long long>(0, std::llround(sampleRate / static_cast<double>(fps)));
    if (formatFrames >= 0) localEnd = std::clamp<long long>(localEnd, localStart, formatFrames);
    const long long frames = std::max<long long>(0, localEnd - localStart);
    const long long absoluteStart = localStart + formatStart;

    const bool interpolate = node.pbool("interpolate", true);
    const bool clampOutput = node.pbool("clamp", true);
    float targets[8] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    float previous[8] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    float firstTarget = 0.0f;
    bool haveTarget = false;
    for (int c = 0; c < channels; ++c) {
        const bool connected = c < static_cast<int>(in.size()) &&
                               in[static_cast<size_t>(c)].type == PortType::Scalar;
        // Legacy single-port chains keep the mono result on every channel.
        float target =
            connected ? in[static_cast<size_t>(c)].scalar : (haveTarget ? firstTarget : 0.0f);
        if (!std::isfinite(target)) target = 0.0f;
        // Without a carrier the Scalar is the sample value itself; with one it
        // is a control value, so clamping happens on the rendered sample.
        if (clampOutput && !carrier) target = std::clamp(target, -1.0f, 1.0f);
        targets[c] = target;
        if (!haveTarget) {
            firstTarget = target;
            haveTarget = true;
        }
        const auto stored = node.runtimeState.find("dac.previous." + std::to_string(c));
        previous[c] = stored != node.runtimeState.end()
                          ? static_cast<float>(stored->second)
                          : target;
    }

    char key[224];
    std::snprintf(key, sizeof(key), "%p|%p|%lld|%lld|%d|%d|%d|%d",
                  static_cast<const void *>(carrier.get()), static_cast<const void *>(format),
                  formatStart, formatFrames, sampleRate, channels, interpolate ? 1 : 0,
                  clampOutput ? 1 : 0);

    const auto fill = [&](AudioBuffer &buffer, long long offset, long long count,
                          const float *from, const float *to, long long localBase) {
        const size_t base = static_cast<size_t>(offset) * static_cast<size_t>(channels);
        const int carrierChannels = carrier ? std::max(1, carrier->channels) : 1;
        for (long long i = 0; i < count; ++i) {
            const float t = interpolate && count > 1
                                ? static_cast<float>(static_cast<double>(i + 1) / count)
                                : 1.0f;
            for (int c = 0; c < channels; ++c) {
                const float control = from[c] + (to[c] - from[c]) * t;
                float sample = control;
                if (carrier) {
                    const int sourceChannel = std::min(c, carrierChannels - 1);
                    const size_t index =
                        static_cast<size_t>(localBase + i) *
                            static_cast<size_t>(carrierChannels) +
                        static_cast<size_t>(sourceChannel);
                    sample = index < carrier->samples.size()
                                 ? carrier->samples[index] * control
                                 : 0.0f;
                }
                if (clampOutput) sample = std::clamp(sample, -1.0f, 1.0f);
                buffer.samples[base + static_cast<size_t>(i) * static_cast<size_t>(channels) +
                               static_cast<size_t>(c)] = sample;
            }
        }
    };

    if (ctx.offline) {
        const bool reusable = node.audioRenderOutput && node.audioRenderKey == key &&
                              absoluteStart >= node.audioRenderStart &&
                              absoluteStart + frames <=
                                  node.audioRenderStart + node.audioRenderFrames;
        if (!reusable) {
            const bool append = node.audioRenderOutput && node.audioRenderKey == key &&
                                absoluteStart ==
                                    node.audioRenderStart + node.audioRenderFrames;
            if (!append) {
                node.audioRenderOutput = std::make_shared<AudioBuffer>();
                node.audioRenderOutput->channels = channels;
                node.audioRenderOutput->sampleRate = sampleRate;
                node.audioRenderOutput->startFrame = absoluteStart;
                node.audioRenderStart = absoluteStart;
                node.audioRenderFrames = 0;
                node.audioRenderKey = key;
            }
            AudioBuffer &buffer = *node.audioRenderOutput;
            const long long offset = node.audioRenderFrames;
            buffer.samples.resize(
                static_cast<size_t>(offset + frames) * static_cast<size_t>(channels));
            fill(buffer, offset, frames, append ? previous : targets, targets, localStart);
            node.audioRenderFrames += frames;
            buffer.frameCount = node.audioRenderFrames;
        }
        out[0].audio = node.audioRenderOutput;
    } else {
        // A buffer holding more than this frame's window was rendered offline
        // (the monitor's pre-rendered soundtrack); do not overwrite it while
        // the editor plays it back.
        const bool preRendered = node.audioRenderOutput && node.audioRenderFrames > frames;
        if (!preRendered) {
            if (!node.audioRenderOutput) node.audioRenderOutput = std::make_shared<AudioBuffer>();
            AudioBuffer &buffer = *node.audioRenderOutput;
            const bool restart = node.audioRenderKey != key ||
                                 absoluteStart !=
                                     node.audioRenderStart + node.audioRenderFrames;
            buffer.channels = channels;
            buffer.sampleRate = sampleRate;
            buffer.startFrame = absoluteStart;
            buffer.frameCount = frames;
            buffer.samples.assign(static_cast<size_t>(frames) *
                                      static_cast<size_t>(channels),
                                  0.0f);
            fill(buffer, 0, frames, restart ? targets : previous, targets, localStart);
            node.audioRenderStart = absoluteStart;
            node.audioRenderFrames = frames;
            node.audioRenderKey = key;
        }
        out[0].audio = node.audioRenderOutput;
    }
    for (int c = 0; c < channels; ++c) {
        node.runtimeState["dac.previous." + std::to_string(c)] =
            static_cast<double>(targets[c]);
    }
}

void evalNoise(Node &node, EvalContext &ctx, const std::vector<Value> &in, std::vector<Value> &out) {
    (void)in;
    const float speed = node.pfloat("speed", 1.0f);
    const double phase = ctx.time * speed;
    double &state = node.runtimeState["noise"];
    const int type = node.pint("type", 0);
    float value = 0.0f;
    switch (type) {
        case 1: {  // sample & hold
            const double cell = std::floor(phase);
            const unsigned int hashed = static_cast<unsigned int>(cell * 2654435761.0) ^
                                        static_cast<unsigned int>(node.pint("seed", 1) * 40503);
            state = static_cast<double>(hashed % 100000u) / 100000.0;
            value = static_cast<float>(state);
            break;
        }
        case 2: {  // pink-ish (sum of octaves)
            float sum = 0.0f;
            float amp = 0.5f;
            for (int octave = 0; octave < 4; ++octave) {
                const double p = phase * static_cast<double>(1 << octave);
                const float x = static_cast<float>(std::sin(p * 12.9898) * 43758.5453);
                sum += (x - std::floor(x) - 0.5f) * amp;
                amp *= 0.6f;
            }
            value = std::clamp(sum + 0.5f, 0.0f, 1.0f);
            break;
        }
        default: {  // smooth value noise
            const double cell = std::floor(phase);
            const float t = static_cast<float>(phase - cell);
            auto hashat = [&](double c) {
                const unsigned int h = static_cast<unsigned int>(c * 1274126177.0) ^
                                       static_cast<unsigned int>(node.pint("seed", 1) * 7919);
                return static_cast<float>((h % 10000u)) / 10000.0f;
            };
            const float a = hashat(cell);
            const float b = hashat(cell + 1.0);
            value = lerp(a, b, t * t * (3.0f - 2.0f * t));
            break;
        }
    }
    value = lerp(0.5f, value, std::clamp(node.pfloat("amount", 1.0f), 0.0f, 1.0f));
    out[0] = Value::makeScalar(std::clamp(value, 0.0f, 1.0f));
}

// ---------------------------------------------------------------------------
// Math
// ---------------------------------------------------------------------------


void setMatrixRow(Matrix &m, int row, float a, float b, float c, float d) {
    switch (row) {
        case 0: m.m0 = a; m.m1 = b; m.m2 = c; m.m3 = d; break;
        case 1: m.m4 = a; m.m5 = b; m.m6 = c; m.m7 = d; break;
        case 2: m.m8 = a; m.m9 = b; m.m10 = c; m.m11 = d; break;
        default: m.m12 = a; m.m13 = b; m.m14 = c; m.m15 = d; break;
    }
}

Vector4 matrixRow(const Matrix &m, int row) {
    switch (row) {
        case 0: return Vector4{m.m0, m.m1, m.m2, m.m3};
        case 1: return Vector4{m.m4, m.m5, m.m6, m.m7};
        case 2: return Vector4{m.m8, m.m9, m.m10, m.m11};
        default: return Vector4{m.m12, m.m13, m.m14, m.m15};
    }
}

float scalarOr(const std::vector<Value> &in, size_t port, float fallback) {
    if (port < in.size() && in[port].type == PortType::Scalar) return in[port].scalar;
    return fallback;
}

float applyOutputShape(Node &node, float value, const char *gainKey = "gain",
                       const char *offsetKey = "offset") {
    value = value * node.pfloat(gainKey, 1.0f) + node.pfloat(offsetKey, 0.0f);
    const float lo = node.pfloat("clampMin", -1.0e6f);
    const float hi = node.pfloat("clampMax", 1.0e6f);
    return std::clamp(value, std::min(lo, hi), std::max(lo, hi));
}

void evalConstant(Node &node, EvalContext &ctx, const std::vector<Value> &in, std::vector<Value> &out) {
    (void)ctx;
    (void)in;
    out[0] = Value::makeScalar(node.pfloat("value", 1.0f));
}

void evalArithmetic(Node &node, EvalContext &ctx, const std::vector<Value> &in, std::vector<Value> &out) {
    (void)ctx;
    const float a = scalarOr(in, 0, 0.0f);
    const float b = scalarOr(in, 1, node.pfloat("bValue", 0.0f));
    float result = 0.0f;
    switch (node.pint("op", 0)) {
        case 0: result = a + b; break;
        case 1: result = a - b; break;
        case 2: result = a * b; break;
        case 3: result = std::fabs(b) < 1e-6f ? 0.0f : a / b; break;
        case 4: result = std::min(a, b); break;
        case 5: result = std::max(a, b); break;
        default: result = std::fabs(b) < 1e-6f ? 0.0f : std::fmod(a, b); break;
    }
    out[0] = Value::makeScalar(applyOutputShape(node, result));
}

void evalPower(Node &node, EvalContext &ctx, const std::vector<Value> &in, std::vector<Value> &out) {
    (void)ctx;
    const float base = scalarOr(in, 0, 0.0f);
    const float exponent = scalarOr(in, 1, node.pfloat("exponent", 2.0f));
    // Negative bases with fractional exponents are NaN in C; mirror instead and
    // keep the sign so the signal stays usable as a modulation source.
    const bool signedPower = node.pbool("signed", true);
    const float magnitude = std::pow(std::max(std::fabs(base), 1.0e-9f), exponent);
    const float result = (signedPower && base < 0.0f) ? -magnitude : magnitude;
    out[0] = Value::makeScalar(applyOutputShape(node, result));
}

void evalExponential(Node &node, EvalContext &ctx, const std::vector<Value> &in, std::vector<Value> &out) {
    (void)ctx;
    const float x = scalarOr(in, 0, 0.0f) * node.pfloat("inputScale", 1.0f);
    const int base = node.pint("base", 0);
    const float result = base == 1 ? std::exp2(x) : (base == 2 ? std::pow(10.0f, x) : std::exp(x));
    out[0] = Value::makeScalar(applyOutputShape(node, result));
}

void evalLogarithm(Node &node, EvalContext &ctx, const std::vector<Value> &in, std::vector<Value> &out) {
    (void)ctx;
    const float floorValue = std::max(1.0e-9f, node.pfloat("floor", 1.0e-4f));
    const float x = std::max(scalarOr(in, 0, floorValue), floorValue);
    const int base = node.pint("base", 0);
    const float result = base == 1 ? std::log2(x) : (base == 2 ? std::log10(x) : std::log(x));
    out[0] = Value::makeScalar(applyOutputShape(node, result));
}

float angleToRadians(float value, int unit) {
    switch (unit) {
        case 1: return value * 6.28318530718f;          // turns
        case 2: return value * 0.0174532925199f;        // degrees
        default: return value;                           // radians
    }
}

float radiansToUnit(float radians, int unit) {
    switch (unit) {
        case 1: return radians / 6.28318530718f;
        case 2: return radians * 57.2957795131f;
        default: return radians;
    }
}

void evalTrig(Node &node, EvalContext &ctx, const std::vector<Value> &in, std::vector<Value> &out) {
    (void)ctx;
    const float raw = scalarOr(in, 0, 0.0f);
    const float angle = angleToRadians(raw * node.pfloat("frequency", 1.0f) +
                                           node.pfloat("phase", 0.0f),
                                       node.pint("unit", 0));
    const float gain = node.pfloat("gain", 1.0f);
    const float offset = node.pfloat("offset", 0.0f);
    out[0] = Value::makeScalar(std::sin(angle) * gain + offset);
    out[1] = Value::makeScalar(std::cos(angle) * gain + offset);
    out[2] = Value::makeScalar(std::clamp(std::tan(angle), -1.0e4f, 1.0e4f) * gain + offset);
}

void evalHyperbolic(Node &node, EvalContext &ctx, const std::vector<Value> &in, std::vector<Value> &out) {
    (void)ctx;
    const float x = scalarOr(in, 0, 0.0f) * node.pfloat("inputScale", 1.0f);
    const float gain = node.pfloat("gain", 1.0f);
    const float offset = node.pfloat("offset", 0.0f);
    out[0] = Value::makeScalar(std::sinh(x) * gain + offset);
    out[1] = Value::makeScalar(std::cosh(x) * gain + offset);
    out[2] = Value::makeScalar(std::tanh(x) * gain + offset);
}

void evalInverseTrig(Node &node, EvalContext &ctx, const std::vector<Value> &in, std::vector<Value> &out) {
    (void)ctx;
    const float x = scalarOr(in, 0, 0.0f);
    const int unit = node.pint("unit", 0);
    const float gain = node.pfloat("gain", 1.0f);
    const float offset = node.pfloat("offset", 0.0f);
    const float clamped = std::clamp(x, -1.0f, 1.0f);
    out[0] = Value::makeScalar(radiansToUnit(std::asin(clamped), unit) * gain + offset);
    out[1] = Value::makeScalar(radiansToUnit(std::acos(clamped), unit) * gain + offset);
    out[2] = Value::makeScalar(radiansToUnit(std::atan(x), unit) * gain + offset);
}

void evalVector2(Node &node, EvalContext &ctx, const std::vector<Value> &in, std::vector<Value> &out) {
    (void)ctx;
    const float x = scalarOr(in, 0, node.pfloat("x", 0.0f));
    const float y = scalarOr(in, 1, node.pfloat("y", 0.0f));
    out[0] = Value::makeVec2(Vector2{x, y});
    out[1] = Value::makeScalar(x);
    out[2] = Value::makeScalar(y);
}

void evalVector3(Node &node, EvalContext &ctx, const std::vector<Value> &in, std::vector<Value> &out) {
    (void)ctx;
    const float x = scalarOr(in, 0, node.pfloat("x", 0.0f));
    const float y = scalarOr(in, 1, node.pfloat("y", 0.0f));
    const float z = scalarOr(in, 2, node.pfloat("z", 0.0f));
    out[0] = Value::makeVec3(Vector3{x, y, z});
    out[1] = Value::makeScalar(x);
    out[2] = Value::makeScalar(y);
    out[3] = Value::makeScalar(z);
}

void evalVector4(Node &node, EvalContext &ctx, const std::vector<Value> &in, std::vector<Value> &out) {
    (void)ctx;
    const float x = scalarOr(in, 0, node.pfloat("x", 0.0f));
    const float y = scalarOr(in, 1, node.pfloat("y", 0.0f));
    const float z = scalarOr(in, 2, node.pfloat("z", 0.0f));
    const float w = scalarOr(in, 3, node.pfloat("w", 0.0f));
    out[0] = Value::makeVec4(Vector4{x, y, z, w});
    out[1] = Value::makeScalar(x);
    out[2] = Value::makeScalar(y);
    out[3] = Value::makeScalar(z);
    out[4] = Value::makeScalar(w);
}

void evalMatrix(Node &node, EvalContext &ctx, const std::vector<Value> &in, std::vector<Value> &out) {
    (void)ctx;
    // The enum index (2x2/3x3/4x4) maps to the dimension as index + 2.
    const int matrixSize = std::clamp(node.pint("size", 1) + 2, 2, 4);
    // Identity fallback when the node has no grid parameter (older projects).
    static const Param kIdentity = [] {
        Param p;
        p.values.assign(16, 0.0f);
        for (int i = 0; i < 4; ++i) p.values[static_cast<size_t>(i * 4 + i)] = 1.0f;
        return p;
    }();
    const Param *grid = node.find("matrix");
    const Param &source = grid ? *grid : kIdentity;
    Matrix matrix{};
    for (int row = 0; row < 4; ++row) {
        setMatrixRow(matrix, row, matrixParamValue(source, row, 0), matrixParamValue(source, row, 1),
                     matrixParamValue(source, row, 2), matrixParamValue(source, row, 3));
    }
    // Connected Vector4 rows overwrite the grid rows.
    for (int row = 0; row < matrixSize; ++row) {
        if (row < static_cast<int>(in.size()) && in[static_cast<size_t>(row)].type == PortType::Vec4) {
            const Vector4 rowValue = in[static_cast<size_t>(row)].vec4;
            setMatrixRow(matrix, row, rowValue.x, rowValue.y, rowValue.z, rowValue.w);
        }
    }
    out[0] = Value::makeMatrix(matrix);
}

float determinant2(const Matrix &m) { return m.m0 * m.m5 - m.m1 * m.m4; }

float determinant3(const Matrix &m) {
    return m.m0 * (m.m5 * m.m10 - m.m6 * m.m9) - m.m1 * (m.m4 * m.m10 - m.m6 * m.m8) +
           m.m2 * (m.m4 * m.m9 - m.m5 * m.m8);
}

float determinant4(const Matrix &m) {
    const float a = m.m0, b = m.m1, c = m.m2, d = m.m3;
    const float e = m.m4, f = m.m5, g = m.m6, h = m.m7;
    const float i = m.m8, j = m.m9, k = m.m10, l = m.m11;
    const float n = m.m12, o = m.m13, p = m.m14, q = m.m15;
    return a * (f * k * q - f * l * p - g * j * q + g * l * o + h * j * p - h * k * o) -
           b * (e * k * q - e * l * p - g * i * q + g * l * n + h * i * p - h * k * n) +
           c * (e * j * q - e * l * o - f * i * q + f * l * n + h * i * o - h * j * n) -
           d * (e * j * p - e * k * o - f * i * p + f * k * n + g * i * o - g * j * n);
}

void evalDeterminant(Node &node, EvalContext &ctx, const std::vector<Value> &in, std::vector<Value> &out) {
    (void)ctx;
    if (in.empty() || in[0].type != PortType::Matrix) {
        out[0] = Value::makeScalar(0.0f);
        out[1] = Value::makeScalar(0.0f);
        return;
    }
    const Matrix &m = in[0].matrix;
    const int size = std::clamp(node.pint("size", 1) + 2, 2, 4);
    float det = 0.0f;
    float trace = 0.0f;
    switch (size) {
        case 2:
            det = determinant2(m);
            trace = m.m0 + m.m5;
            break;
        case 4:
            det = determinant4(m);
            trace = m.m0 + m.m5 + m.m10 + m.m15;
            break;
        default:
            det = determinant3(m);
            trace = m.m0 + m.m5 + m.m10;
            break;
    }
    out[0] = Value::makeScalar(det);
    out[1] = Value::makeScalar(trace);
}

// ---------------------------------------------------------------------------
// Modulation
// ---------------------------------------------------------------------------

void evalLfo(Node &node, EvalContext &ctx, const std::vector<Value> &in, std::vector<Value> &out) {
    const bool frequencyConnected = in.size() > 1 && in[1].type == PortType::Scalar;
    const bool amplitudeConnected = in.size() > 2 && in[2].type == PortType::Scalar;
    const bool offsetConnected = in.size() > 3 && in[3].type == PortType::Scalar;
    float frequency = node.pfloat("frequency", 1.0f);
    if (frequencyConnected) frequency *= std::pow(2.0f, std::clamp(in[1].scalar, -8.0f, 8.0f));
    frequency = std::clamp(frequency, 0.0f, 200.0f);
    const float phaseOffset = node.pfloat("phase", 0.0f);
    const bool sync = node.pbool("sync", false);
    double phase = 0.0;
    if (sync) {
        const float bpm = std::max(1.0f, node.pfloat("bpm", 120.0f));
        static const float divisors[] = {1.0f, 2.0f, 4.0f, 8.0f, 16.0f};
        const float cycles = bpm / 60.0f / divisors[std::clamp(node.pint("division", 1), 0, 4)];
        phase = ctx.time * cycles;
    } else {
        phase = ctx.time * frequency;
    }
    phase += phaseOffset;
    if (in.size() > 0 && in[0].type == PortType::Scalar) phase += in[0].scalar;
    const float t = static_cast<float>(phase - std::floor(phase));
    float wave = 0.0f;
    switch (node.pint("shape", 0)) {
        case 0: wave = std::sin(t * 6.2831853f); break;
        case 1: wave = 4.0f * std::fabs(t - 0.5f) - 1.0f; break;
        case 2: wave = t * 2.0f - 1.0f; break;
        case 3: wave = 1.0f - t * 2.0f; break;
        case 4: wave = t < 0.5f ? 1.0f : -1.0f; break;
        case 5: {  // random step
            const double cell = std::floor(phase);
            const unsigned int hashed = static_cast<unsigned int>(cell * 2654435761.0);
            wave = static_cast<float>(hashed % 10000u) / 5000.0f - 1.0f;
            break;
        }
        default: wave = std::sin(t * 6.2831853f) * 0.5f; break;
    }
    float amplitude = node.pfloat("amplitude", 1.0f);
    if (amplitudeConnected) amplitude *= std::max(0.0f, 1.0f + in[2].scalar);
    float offset = node.pfloat("offset", 0.0f);
    if (offsetConnected) offset += in[3].scalar;
    // Keep the phase so the block preview can put its pivot on the waveform.
    node.runtimeState["phase"] = t;
    node.publishEffective("frequency", frequency);
    node.publishEffective("amplitude", amplitude);
    node.publishEffective("offset", offset);
    out[0] = Value::makeScalar(wave * amplitude + offset);
}

void evalAutomation(Node &node, EvalContext &ctx, const std::vector<Value> &in, std::vector<Value> &out) {
    const bool depthConnected = !in.empty() && in[0].type == PortType::Scalar;
    const bool offsetConnected = in.size() > 1 && in[1].type == PortType::Scalar;
    double normalized = ctx.duration > 0.0 ? ctx.time / ctx.duration : 0.0;
    if (node.pbool("loop", false)) {
        const double span = std::max(1e-4, static_cast<double>(node.pfloat("loopLength", 1.0f)));
        normalized = (normalized / span) - std::floor(normalized / span);
    }
    normalized = std::clamp(normalized, 0.0, 1.0);
    // The curve is stored in 0..1 curve space; the bipolar switch decides whether
    // that maps to 0..1 or to -1..1 before depth/offset are applied.
    const float curve = std::clamp(node.curveAt("curve", normalized), 0.0f, 1.0f);
    node.runtimeState["pos"] = normalized;
    float result = node.pbool("bipolar", false) ? (curve * 2.0f - 1.0f) : curve;
    float depth = node.pfloat("depth", 1.0f);
    if (depthConnected) depth *= std::max(0.0f, 1.0f + in[0].scalar);
    float offset = node.pfloat("offset", 0.0f);
    if (offsetConnected) offset += in[1].scalar;
    node.publishEffective("depth", depth);
    node.publishEffective("offset", offset);
    result = result * depth + offset;
    if (node.pbool("smooth", false)) {
        result = follow(node, "smooth", result, node.pfloat("smoothing", 0.2f),
                        node.pfloat("smoothing", 0.2f), dtOf(ctx));
    }
    out[0] = Value::makeScalar(result);
}

void evalAmount(Node &node, EvalContext &ctx, const std::vector<Value> &in, std::vector<Value> &out) {
    (void)ctx;
    const float value = scalarFrom(in, 0);
    const bool hasAmount = in.size() > 1 && in[1].type == PortType::Scalar;
    const bool gainConnected = in.size() > 2 && in[2].type == PortType::Scalar;
    const bool offsetConnected = in.size() > 3 && in[3].type == PortType::Scalar;
    float amount = hasAmount ? in[1].scalar : node.pfloat("amount", 1.0f);
    amount = std::pow(std::clamp(amount, 0.0f, 4.0f), std::max(0.05f, node.pfloat("curve", 1.0f)));
    float gain = node.pfloat("gain", 1.0f);
    if (gainConnected) gain *= std::max(0.0f, 1.0f + in[2].scalar);
    float offset = node.pfloat("offset", 0.0f);
    if (offsetConnected) offset += in[3].scalar;
    node.publishEffective("gain", gain);
    node.publishEffective("offset", offset);
    float result = value * amount * gain + offset;
    const int steps = node.pint("quantize", 0);
    if (steps > 1) result = std::round(result * static_cast<float>(steps)) / static_cast<float>(steps);
    out[0] = Value::makeScalar(result);
}

// Ringbuffer: records the incoming scalar and hands back the live value, the
// running average of the buffer and whatever the looping read pointer is over.
void evalRingbuffer(Node &node, EvalContext &ctx, const std::vector<Value> &in,
                    std::vector<Value> &out) {
    const float value = scalarFrom(in, 0);
    const int size = std::clamp(node.pint("size", 64), 2, 1024);
    // O(1) ring: push, running sum and read pointer all avoid the old O(size)
    // shift/sum, which was too slow inside an audio-rate region.
    if (static_cast<int>(node.ringValues.size()) != size) {
        node.ringValues.assign(static_cast<size_t>(size), 0.0f);
        node.ringHead = 0;
        node.ringCount = 0;
        node.ringSum = 0.0;
    }
    if (node.ringCount < size) {
        const int write = (node.ringHead + node.ringCount) % size;
        node.ringValues[static_cast<size_t>(write)] = value;
        node.ringSum += value;
        ++node.ringCount;
    } else {
        node.ringSum += value - node.ringValues[static_cast<size_t>(node.ringHead)];
        node.ringValues[static_cast<size_t>(node.ringHead)] = value;
        node.ringHead = (node.ringHead + 1) % size;
    }
    const int count = std::max(1, node.ringCount);
    const float average = static_cast<float>(node.ringSum / count);

    // Read pointer: `speed` loops per second, advanced one evaluation at a time
    // so the movement is independent of the evaluation rate (video frame rate
    // normally, audio sample rate inside an ADC -> DAC region).
    const float fs = rateOf(ctx);
    float speed = node.pfloat("speed", 0.5f);
    if (in.size() > 1 && in[1].type == PortType::Scalar) {
        speed *= std::pow(2.0f, std::clamp(in[1].scalar, -8.0f, 8.0f));
    }
    speed = std::clamp(speed, 0.001f, 64.0f);
    node.publishEffective("speed", speed);
    double phase = node.runtimeState["phase"] + speed / fs;
    phase -= std::floor(phase);
    node.runtimeState["phase"] = phase;
    const int readIndex = std::clamp(static_cast<int>(phase * count), 0, count - 1);
    const int physical = (node.ringHead + readIndex) % size;
    const float buffered = node.ringValues[static_cast<size_t>(physical)];

    // The in-block display is a video-rate view: at audio rate push one sample
    // per video frame instead of one per audio sample.
    const bool pushVisual =
        !ctx.audioRate || node.runtimeState["ring.visualFrame"] !=
                              static_cast<double>(ctx.frame);
    if (pushVisual) {
        Node::pushHistory(node.historyA, value, size);
        ++node.historyCount;
        node.runtimeState["ring.visualFrame"] = static_cast<double>(ctx.frame);
    }

    out[0] = Value::makeScalar(value);
    out[1] = Value::makeScalar(average);
    out[2] = Value::makeScalar(buffered);
}

// Signal Filter: RBJ biquad in transposed direct form II. Outside an audio-rate
// region the modulation runs at the video frame rate with fs = fps; inside one
// it processes every audio sample with fs = sample rate.
void evalSignalFilter(Node &node, EvalContext &ctx, const std::vector<Value> &in,
                      std::vector<Value> &out) {
    const double input = scalarFrom(in, 0);
    const double fs = rateOf(ctx);
    const int mode = std::clamp(node.pint("mode", 0), 0, 2);
    // Modulation inputs shift cutoff and resonance in octaves (x2 per unit), the
    // musical way to sweep a filter.
    double q = std::clamp(node.pfloat("resonance", 0.707f), 0.05f, 20.0f);
    if (in.size() > 2 && in[2].type == PortType::Scalar) {
        q *= std::pow(2.0, std::clamp(static_cast<double>(in[2].scalar), -8.0, 8.0));
    }
    q = std::clamp(q, 0.05, 20.0);
    double cutoff = node.pfloat("cutoff", 4.0f);
    if (in.size() > 1 && in[1].type == PortType::Scalar) {
        cutoff *= std::pow(2.0, std::clamp(static_cast<double>(in[1].scalar), -8.0, 8.0));
    }
    cutoff = std::clamp(cutoff, 0.001, std::max(1.0, fs * 0.49));
    node.publishEffective("cutoff", static_cast<float>(cutoff));
    node.publishEffective("resonance", static_cast<float>(q));
    const BiquadCoefficients coefficients = biquadCoefficients(mode, cutoff, q, fs);

    double z1 = node.runtimeState["z1"];
    double z2 = node.runtimeState["z2"];
    const double output = coefficients.b0 * input + z1;
    z1 = coefficients.b1 * input - coefficients.a1 * output + z2;
    z2 = coefficients.b2 * input - coefficients.a2 * output;
    node.runtimeState["z1"] = z1;
    node.runtimeState["z2"] = z2;
    out[0] = Value::makeScalar(static_cast<float>(output));
}

// ---------------------------------------------------------------------------
// Debug
// ---------------------------------------------------------------------------

// VU / Digital Meter: the value passes through unchanged, the movement is what
// gets displayed. 0 VU sits at -18 dBFS.
void evalMeter(Node &node, EvalContext &ctx, const std::vector<Value> &in,
               std::vector<Value> &out) {
    (void)ctx;
    const float value = scalarFrom(in, 0);
    out[0] = Value::makeScalar(value);

    const double magnitude = std::fabs(static_cast<double>(value));
    const float dbfs = magnitude > 1.0e-6 ? static_cast<float>(20.0 * std::log10(magnitude))
                                          : -120.0f;
    const float vu = dbfs + 18.0f;  // 0 VU = -18 dBFS
    const float release = std::clamp(node.pfloat("ballistics", 0.55f), 0.0f, 0.95f);
    double needle = node.runtimeState["needle"];
    needle = vu >= needle ? vu : needle * release + vu * (1.0 - release);
    node.runtimeState["needle"] = needle;
    node.runtimeState["vu"] = vu;
    node.runtimeState["db"] = dbfs;
    node.runtimeState["peak"] = std::max<double>(vu, node.runtimeState["peak"] * 0.99);
    Node::pushHistory(node.historyA, value, kMeterHistory);
    ++node.historyCount;
}

// Guard: non-finite scalars are silenced and the matching lamp is lit. The lamps
// decay so a single event stays visible for a moment.
void evalGuard(Node &node, EvalContext &ctx, const std::vector<Value> &in,
               std::vector<Value> &out) {
    const float value = scalarFrom(in, 0);
    const bool isNan = std::isnan(value);
    const bool positiveInf = std::isinf(value) && value > 0.0f;
    const bool negativeInf = std::isinf(value) && value < 0.0f;
    const double fade = std::exp(-static_cast<double>(dtOf(ctx)) / 0.25);
    auto lamp = [&](const char *key, bool seen) {
        node.runtimeState[key] = seen ? 1.0 : node.runtimeState[key] * fade;
    };
    lamp("nan", isNan);
    lamp("pos", positiveInf);
    lamp("neg", negativeInf);

    const bool blocked = isNan || positiveInf || negativeInf;
    node.runtimeState["blocked"] = blocked ? 1.0 : 0.0;
    out[0] = Value::makeScalar(blocked ? 0.0f : value);
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

// Spectrum: Analysis in, Image out. The built-in effects read the analysis
// textures the renderer fills in every frame, so this block only needs to know
// that an analysis is connected; its parameters can be driven by the Scale,
// Feedback and Colour inputs.
void evalSpectrum(Node &node, EvalContext &ctx, const std::vector<Value> &in,
                  std::vector<Value> &out) {
    if (!ctx.renderer || !ctx.shaders) return;
    const bool connected = !in.empty() && in[0].type == PortType::Analysis;
    if (!connected) {
        node.status = "connect an Analysis input";
        return;
    }
    if (!in[0].analysis) {
        node.status = "no audio loaded";
        return;
    }
    const std::vector<std::string> presets = ShaderLibrary::effectNames();
    const int index = std::clamp(node.pint("preset", 0), 0,
                                 std::max(0, static_cast<int>(presets.size()) - 1));
    Shader *shader = ctx.shaders->builtin(presets[static_cast<size_t>(index)]);
    if (!shader) {
        node.status = ctx.shaders->lastError();
        return;
    }
    node.status.clear();

    // Modulation inputs scale the parameters they are named after.
    const bool scaleConnected = in.size() > 1 && in[1].type == PortType::Scalar;
    const bool feedbackConnected = in.size() > 2 && in[2].type == PortType::Scalar;
    float scale = node.pfloat("scale", 1.0f);
    if (scaleConnected) scale *= std::max(0.0f, 1.0f + in[1].scalar);
    scale = std::clamp(scale, 0.15f, 4.0f);
    // Quantise the modulated resolution so a sweeping Scale input cannot
    // allocate a new render target (and feedback texture) every frame.
    scale = std::round(scale * 10.0f) / 10.0f;
    float feedbackAmount = node.pfloat("feedback", 0.6f);
    if (feedbackConnected) feedbackAmount += in[2].scalar * 0.5f;
    feedbackAmount = std::clamp(feedbackAmount, 0.0f, 0.98f);
    Color colorA = node.pcolor("colorA");
    Color colorB = node.pcolor("colorB");
    if (in.size() > 3 && in[3].type == PortType::Color) colorA = in[3].color;
    if (in.size() > 4 && in[4].type == PortType::Color) colorB = in[4].color;

    const auto align32 = [](int value) { return std::max(32, (value + 31) & ~31); };
    const int width = align32(static_cast<int>(ctx.width * scale));
    const int height = align32(static_cast<int>(ctx.height * scale));

    float user[8] = {0};
    Texture2D feedback{};
    ImageBufferPtr feedbackTarget;
    if (node.pbool("useFeedback", false)) {
        // Feedback lives at the base (unmodulated) resolution so Scale
        // modulation never reallocates the persistent texture.
        float baseScale = std::clamp(node.pfloat("scale", 1.0f), 0.15f, 4.0f);
        baseScale = std::round(baseScale * 10.0f) / 10.0f;
        feedbackTarget = ctx.renderer->persistent(
            node.id, align32(static_cast<int>(ctx.width * baseScale)),
            align32(static_cast<int>(ctx.height * baseScale)));
        if (feedbackTarget) {
            feedback = feedbackTarget->texture.texture;
            user[0] = std::max(user[0], feedbackAmount);
        }
    }

    ImageBufferPtr target = ctx.renderer->acquire(width, height);
    if (!target) {
        node.status = "out of render targets";
        return;
    }
    ctx.renderer->beginTarget(target, true, Color{0, 0, 0, 255});
    // The block displays the analysis that reaches its port, not the project's
    // imported one.
    ctx.renderer->uploadAnalysisTextures(*in[0].analysis, ctx.audioTime);
    ctx.renderer->drawShaderPass(shader, ctx, Texture2D{}, Texture2D{}, feedback, user, 8, colorA,
                                 colorB, ShaderVectorUniforms{});
    ctx.renderer->endTarget();

    if (feedbackTarget) {
        ctx.renderer->beginTarget(feedbackTarget, false, BLANK);
        ctx.renderer->blit(target);
        ctx.renderer->endTarget();
    }
    out[0] = Value::makeImage(target);
}

// Shader: applies a .glsl file to the incoming image. The node's ports are
// derived from that file, so the uniforms are bound by port name.
void evalShader(Node &node, EvalContext &ctx, const std::vector<Value> &in,
                std::vector<Value> &out) {
    if (!ctx.renderer || !ctx.shaders) return;
    const std::string file = node.pstr("shader");
    auto passThrough = [&]() {
        if (!in.empty() && in[0].image) out[0] = in[0];
    };
    if (file.empty()) {
        node.status = "no .glsl file selected";
        passThrough();
        return;
    }
    std::string error;
    Shader *shader = ctx.shaders->get(file, &error);
    if (!shader) {
        // Never fall back silently to a preset: the block says what went wrong.
        node.status = error.empty() ? ctx.shaders->lastError() : error;
        passThrough();
        return;
    }
    node.status.clear();

    const std::vector<PortDesc> &ports = node.inputPorts();
    float user[8] = {0};
    ShaderVectorUniforms vectors;
    Color colorA = node.pcolor("colorA");
    Color colorB = node.pcolor("colorB");
    Texture2D prev{};
    Texture2D input2{};
    for (size_t port = 0; port < ports.size() && port < in.size(); ++port) {
        const std::string &name = ports[port].name;
        if (port == 0 && in[port].image) {
            prev = in[port].image->texture.texture;
            continue;
        }
        if (name == "uInput2" && in[port].image) {
            input2 = in[port].image->texture.texture;
        } else if (name.rfind("uUser[", 0) == 0 && in[port].type == PortType::Scalar) {
            const int slot = std::atoi(name.c_str() + 6);
            if (slot >= 0 && slot < 8) user[slot] = in[port].scalar;
        } else if (name == "uColorA" && in[port].type == PortType::Color) {
            colorA = in[port].color;
        } else if (name == "uColorB" && in[port].type == PortType::Color) {
            colorB = in[port].color;
        } else if (name == "uVector2" && in[port].type == PortType::Vec2) {
            vectors.vec2 = in[port].vec2;
            vectors.useVec2 = true;
        } else if (name == "uVector3" && in[port].type == PortType::Vec3) {
            vectors.vec3 = in[port].vec3;
            vectors.useVec3 = true;
        } else if (name == "uVector4" && in[port].type == PortType::Vec4) {
            vectors.vec4 = in[port].vec4;
            vectors.useVec4 = true;
        } else if (name == "uMatrix" && in[port].type == PortType::Matrix) {
            vectors.matrix = in[port].matrix;
            vectors.useMatrix = true;
        }
    }

    float scale = std::clamp(node.pfloat("scale", 1.0f), 0.15f, 4.0f);
    const int width = std::max(2, static_cast<int>(ctx.width * scale));
    const int height = std::max(2, static_cast<int>(ctx.height * scale));
    const float feedbackAmount =
        node.pbool("useFeedback", false) ? std::clamp(node.pfloat("feedback", 0.6f), 0.0f, 0.98f)
                                         : 0.0f;
    (void)feedbackAmount;  // the shader reads uFeedback and decides the mix itself
    Texture2D feedback{};
    ImageBufferPtr feedbackTarget;
    if (node.pbool("useFeedback", false)) {
        feedbackTarget = ctx.renderer->persistent(node.id, width, height);
        if (feedbackTarget) feedback = feedbackTarget->texture.texture;
    }

    ImageBufferPtr target = ctx.renderer->acquire(width, height);
    if (!target) {
        node.status = "out of render targets";
        passThrough();
        return;
    }
    ctx.renderer->beginTarget(target, true, Color{0, 0, 0, 255});
    ctx.renderer->drawShaderPass(shader, ctx, prev, input2, feedback, user, 8, colorA, colorB,
                                 vectors);
    ctx.renderer->endTarget();
    if (feedbackTarget) {
        ctx.renderer->beginTarget(feedbackTarget, false, BLANK);
        ctx.renderer->blit(target);
        ctx.renderer->endTarget();
    }
    out[0] = Value::makeImage(target);
}

void evalBlend(Node &node, EvalContext &ctx, const std::vector<Value> &in, std::vector<Value> &out) {
    if (!ctx.renderer || !ctx.shaders) return;
    Shader *shader = ctx.shaders->builtin("blend");
    if (!shader) return;
    float user[8] = {0};
    user[0] = static_cast<float>(node.pint("mode", 1));
    user[1] = node.pfloat("opacity", 1.0f);
    ImageBufferPtr target = ctx.renderer->acquire(ctx.width, ctx.height);
    if (!target) return;
    ctx.renderer->beginTarget(target, true, Color{0, 0, 0, 255});
    ctx.renderer->drawShaderPass(shader, ctx, textureFrom(in, 0), textureFrom(in, 1), Texture2D{},
                                 user, 2, WHITE, WHITE);
    ctx.renderer->endTarget();
    out[0] = Value::makeImage(target);
}

void evalPostFx(Node &node, EvalContext &ctx, const std::vector<Value> &in, std::vector<Value> &out) {
    if (!ctx.renderer || !ctx.shaders) return;
    Shader *shader = ctx.shaders->builtin("postfx");
    if (!shader) return;
    // Scalar inputs override the matching parameter, so any modulation block can
    // drive the grade in real time.
    auto pick = [&](size_t port, const char *key, float fallback, float lo, float hi) {
        if (port < in.size() && in[port].type == PortType::Scalar) {
            return std::clamp(in[port].scalar, lo, hi);
        }
        return std::clamp(node.pfloat(key, fallback), lo, hi);
    };
    const float bloom = pick(1, "bloom", 0.35f, 0.0f, 1.0f);
    const float chromatic = pick(2, "chromatic", 0.15f, 0.0f, 1.0f);
    const float vignette = pick(3, "vignette", 0.35f, 0.0f, 1.0f);
    const float grain = pick(4, "grain", 0.08f, 0.0f, 1.0f);
    const float scanlines = pick(5, "scanlines", 0.0f, 0.0f, 1.0f);
    const float feedbackAmount = pick(6, "feedback", 0.0f, 0.0f, 0.98f);
    const float saturation = pick(7, "saturation", 1.05f, 0.0f, 2.0f);
    const float hue = pick(8, "hue", 0.0f, -1.0f, 1.0f);
    Texture2D feedback{};
    ImageBufferPtr feedbackTarget;
    if (feedbackAmount > 0.001f) {
        feedbackTarget = ctx.renderer->persistent(node.id, ctx.width, ctx.height);
        if (feedbackTarget) feedback = feedbackTarget->texture.texture;
    }
    float user[8] = {bloom, chromatic, vignette, grain, scanlines, feedbackAmount, saturation, hue};
    ImageBufferPtr target = ctx.renderer->acquire(ctx.width, ctx.height);
    if (!target) return;
    ctx.renderer->beginTarget(target, true, Color{0, 0, 0, 255});
    ctx.renderer->drawShaderPass(shader, ctx, textureFrom(in, 0), Texture2D{}, feedback, user, 8,
                                 WHITE, WHITE);
    ctx.renderer->endTarget();
    if (feedbackTarget) {
        ctx.renderer->beginTarget(feedbackTarget, false, BLANK);
        ctx.renderer->blit(target);
        ctx.renderer->endTarget();
    }
    out[0] = Value::makeImage(target);
}

void evalGeometry(Node &node, EvalContext &ctx, const std::vector<Value> &in, std::vector<Value> &out) {
    if (!ctx.renderer) return;

    ImageBufferPtr target = ctx.renderer->acquire(ctx.width, ctx.height);
    if (!target) return;
    ctx.renderer->beginTarget(target, true, Color{0, 0, 0, 255});
    if (in.size() > 0 && in[0].image && in[0].image->valid()) ctx.renderer->blit(in[0].image);

    geometry::GeomSpec spec;
    spec.width = ctx.width;
    spec.height = ctx.height;
    spec.cx = node.pfloat("x", 0.5f) + scalarFrom(in, 3) * node.pfloat("xMod", 0.25f);
    spec.cy = node.pfloat("y", 0.5f) + scalarFrom(in, 4) * node.pfloat("yMod", 0.0f);
    // A Vector2 input overrides the position outright, which is how the Vector
    // blocks drive geometry.
    if (in.size() > 5 && in[5].type == PortType::Vec2) {
        spec.cx = in[5].vec2.x;
        spec.cy = in[5].vec2.y;
    }
    spec.radius = std::max(0.01f, node.pfloat("radius", 0.28f) *
                                      (1.0f + scalarFrom(in, 1) * node.pfloat("scaleMod", 0.5f)));
    spec.thickness = node.pfloat("thickness", 4.0f);
    spec.rotation = node.pfloat("rotation", 0.0f) * 6.2831853f +
                    static_cast<float>(ctx.time) * node.pfloat("spin", 0.0f) +
                    scalarFrom(in, 2) * node.pfloat("rotationMod", 0.0f);
    spec.count = std::max(2, node.pint("count", 64));
    spec.colorA = node.pcolor("colorA");
    spec.colorB = node.pcolor("colorB");
    spec.alpha = std::clamp(node.pfloat("alpha", 1.0f), 0.0f, 1.0f);
    spec.additive = node.pbool("additive", true);
    spec.text = node.pstr("text");
    spec.textSize = node.pfloat("textSize", 72.0f);
    spec.stateKey = node.id;
    spec.dt = dtOf(ctx);
    spec.time = ctx.time;

    const int shapeIndex = std::clamp(node.pint("shape", 0), 0, static_cast<int>(geometry::Shape::Count) - 1);
    geometry::drawPrimitive(static_cast<geometry::Shape>(shapeIndex), spec);
    ctx.renderer->endTarget();
    out[0] = Value::makeImage(target);
}

void evalOutput(Node &node, EvalContext &ctx, const std::vector<Value> &in, std::vector<Value> &out) {
    (void)ctx;
    (void)out;
    if (in.size() > 0 && in[0].image) {
        node.status = "receiving";
    } else {
        node.status = "not connected";
    }
}

void evalAudioOutput(Node &node, EvalContext &ctx, const std::vector<Value> &in,
                     std::vector<Value> &out) {
    (void)ctx;
    (void)out;
    node.status = (in.size() > 0 && in[0].audio) ? "receiving" : "not connected";
}

// ---------------------------------------------------------------------------
// Parameter and port builders
// ---------------------------------------------------------------------------

Param colorParam(const char *key, const char *label, unsigned int hex, const std::string &group) {
    return makeColorParam(key, label, palette::fromHex(hex), group);
}

std::vector<Param> shaderScalarPorts(std::vector<PortDesc> &inputs) {
    for (int i = 0; i < 8; ++i) {
        inputs.push_back(PortDesc{"u" + std::to_string(i), PortType::Scalar, "uniform"});
    }
    return {};
}

}  // namespace

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

Registry &Registry::instance() {
    static Registry registry;
    return registry;
}

Registry::Registry() { registerBuiltins(); }

void Registry::add(NodeDef def) { definitions_.push_back(std::move(def)); }

const NodeDef *Registry::find(const std::string &kind) const {
    for (const auto &def : definitions_) {
        if (def.kind == kind) return &def;
    }
    const std::string modern = modernKindFor(kind);
    if (!modern.empty() && modern != kind) return find(modern);
    return nullptr;
}

std::string Registry::modernKindFor(const std::string &kind) {
    // "Shader Pass" was split into a built-in Spectrum generator and a Shader
    // block that applies a .glsl file. Old projects are migrated on load; the
    // generic alias keeps anything else referencing the old name working.
    if (kind == "shader.pass") return "render.spectrum";
    return kind;
}

bool Registry::applyShaderPorts(Node &node, const ShaderLibrary &shaders, std::string *error) {
    if (node.kind != "render.shader") return false;
    const std::string file = node.pstr("shader");
    if (file.empty()) {
        node.clearInputPorts();  // the definition default: a single uPrev image
        return true;
    }
    std::vector<ShaderInput> inputs;
    if (!shaders.describeInputs(file, &inputs, error)) {
        node.clearInputPorts();
        return false;
    }
    std::vector<PortDesc> ports;
    ports.reserve(inputs.size());
    for (const ShaderInput &input : inputs) {
        ports.push_back(PortDesc{input.name, input.type, input.uniform});
    }
    if (ports.empty()) ports.push_back(PortDesc{"uPrev", PortType::Image, "uPrev"});
    node.setInputPorts(std::move(ports));
    return true;
}

void Registry::applyChannelPorts(Node &node) {
    if (node.kind != "dsp.adc" && node.kind != "dsp.dac") return;
    const int channels = std::clamp(node.pint("channels", 2), 1, 8);
    std::vector<PortDesc> ports;
    ports.reserve(static_cast<size_t>(channels));
    for (int i = 0; i < channels; ++i) {
        std::string name;
        if (i == 0) name = "left";
        else if (i == 1) name = "right";
        else name = "ch" + std::to_string(i + 1);
        ports.push_back(PortDesc{name, PortType::Scalar, "sample"});
    }
    if (node.kind == "dsp.adc") {
        node.setOutputPorts(std::move(ports));
    } else {
        node.setInputPorts(std::move(ports));
    }
}

std::vector<const NodeDef *> Registry::byCategory(const std::string &category) const {
    std::vector<const NodeDef *> result;
    for (const auto &def : definitions_) {
        if (def.category == category) result.push_back(&def);
    }
    return result;
}

std::vector<std::string> Registry::categories() const {
    std::vector<std::string> result;
    for (const auto &def : definitions_) {
        if (std::find(result.begin(), result.end(), def.category) == result.end()) {
            result.push_back(def.category);
        }
    }
    return result;
}

void Registry::registerBuiltins() {
    // ---- Source ----------------------------------------------------------
    {
        NodeDef def;
        def.kind = "src.audio";
        def.category = "Source";
        def.label = "Audio Source";
        def.description =
            "Emits the project's decoded audio clip. Any format ffmpeg understands is "
            "accepted and the whole file is decoded to 32-bit float on load.";
        def.outputs = {PortDesc{"Audio", PortType::Audio, "decoded clip"}};
        def.evaluate = evalAudioSource;
        add(std::move(def));
    }

    // ---- Timing ----------------------------------------------------------
    {
        NodeDef def;
        def.kind = "time.clock";
        def.category = "Timing";
        def.label = "Time Source";
        def.description = "Video timing: emits seconds, frame index, normalised progress and beat phase.";
        def.outputs = {PortDesc{"Time", PortType::Scalar, "seconds"},
                       PortDesc{"Frame", PortType::Scalar, "frame index"},
                       PortDesc{"Progress", PortType::Scalar, "0..1"},
                       PortDesc{"Beat", PortType::Scalar, "beat phase 0..1"}};
        def.params = {
            makeParam("speed", "Speed", 1.0f, 0.0f, 4.0f, 0.01f, "Time"),
            makeParam("offset", "Offset (s)", 0.0f, -30.0f, 30.0f, 0.01f, "Time"),
            makeParam("bpm", "Tempo (BPM)", 120.0f, 20.0f, 300.0f, 1.0f, "Beat"),
        };
        def.evaluate = evalClock;
        add(std::move(def));
    }
    {
        NodeDef def;
        def.kind = "time.pulse";
        def.category = "Timing";
        def.label = "Beat Pulse";
        def.description =
            "Decaying trigger on a musical division, handy for stabs and envelopes. Tempo, decay "
            "and offset can be modulated; the tempo input shifts the rate by octaves.";
        def.inputs = {PortDesc{"Tempo", PortType::Scalar, "BPM x 2^input"},
                      PortDesc{"Decay", PortType::Scalar, "scales the decay"},
                      PortDesc{"Offset", PortType::Scalar, "shifts the phase by 0.25 s per unit"}};
        def.outputs = {PortDesc{"Pulse", PortType::Scalar, "0..1"}};
        def.params = {
            makeParam("bpm", "Tempo (BPM)", 120.0f, 20.0f, 300.0f, 1.0f, "Pulse"),
            makeEnumParam("division", "Division", {"1/1", "1/2", "1/4", "1/8", "1/16"}, 2, "Pulse"),
            makeParam("decay", "Decay", 0.5f, 0.02f, 2.0f, 0.01f, "Pulse"),
            makeParam("offset", "Offset (s)", 0.0f, -10.0f, 10.0f, 0.01f, "Pulse"),
        };
        def.evaluate = evalPulse;
        add(std::move(def));
    }

    // ---- DSP -------------------------------------------------------------
    {
        NodeDef def;
        def.kind = "dsp.analyze";
        def.category = "DSP";
        def.label = "Spectrum Analyzer";
        def.description =
            "Analyses the Audio wired into this port: the imported clip uses its whole-file "
            "analysis, any other stream (Dynamics, DAC, ...) is measured live at frame rate. "
            "Emits band magnitudes plus level, onset and bass/mid/treble scalars.";
        def.inputs = {PortDesc{"Audio", PortType::Audio}};
        def.outputs = {PortDesc{"Analysis", PortType::Analysis},
                       PortDesc{"Level", PortType::Scalar},
                       PortDesc{"Onset", PortType::Scalar},
                       PortDesc{"Bass", PortType::Scalar},
                       PortDesc{"Mid", PortType::Scalar},
                       PortDesc{"Treble", PortType::Scalar}};
        def.params = {
            makeParam("gain", "Gain", 1.0f, 0.0f, 8.0f, 0.01f, "Output"),
            makeParam("gate", "Gate", 0.0f, 0.0f, 0.5f, 0.001f, "Output"),
        };
        def.evaluate = evalAnalyzer;
        add(std::move(def));
    }
    {
        NodeDef def;
        def.kind = "dsp.band";
        def.category = "DSP";
        def.label = "Frequency Band";
        def.description = "Selects a frequency range from the analysis and turns it into a scalar.";
        def.inputs = {PortDesc{"Analysis", PortType::Analysis}};
        def.outputs = {PortDesc{"Value", PortType::Scalar}};
        // Logarithmic: the ear and the analysis bands are both log-spaced.
        Param lowHz = makeParam("lowHz", "Low (Hz)", 40.0f, 20.0f, 16000.0f, 1.0f, "Band", true);
        Param highHz = makeParam("highHz", "High (Hz)", 250.0f, 21.0f, 20000.0f, 1.0f, "Band", true);
        lowHz.valueFormat = "%.0f Hz";
        highHz.valueFormat = "%.0f Hz";
        def.params = {
            lowHz,
            highHz,
            makeEnumParam("mode", "Mode", {"Average", "Peak", "Sum"}, 0, "Band"),
            makeParam("gain", "Gain", 1.0f, 0.0f, 8.0f, 0.01f, "Shape"),
            makeParam("curve", "Curve", 1.0f, 0.05f, 4.0f, 0.01f, "Shape"),
            makeParam("gate", "Gate", 0.0f, 0.0f, 0.5f, 0.001f, "Shape"),
            makeParam("attack", "Attack", 0.5f, 0.001f, 1.0f, 0.001f, "Envelope"),
            makeParam("release", "Release", 0.12f, 0.001f, 1.0f, 0.001f, "Envelope"),
        };
        def.evaluate = evalBand;
        add(std::move(def));
    }
    {
        NodeDef def;
        def.kind = "dsp.env";
        def.category = "DSP";
        def.label = "Envelope Follower";
        def.description = "Attack/release smoothing, gain and floor for any scalar signal.";
        def.inputs = {PortDesc{"Value", PortType::Scalar}};
        def.outputs = {PortDesc{"Value", PortType::Scalar}};
        def.params = {
            makeParam("attack", "Attack", 0.4f, 0.001f, 1.0f, 0.001f, "Envelope"),
            makeParam("release", "Release", 0.1f, 0.001f, 1.0f, 0.001f, "Envelope"),
            makeParam("threshold", "Threshold", 0.0f, 0.0f, 1.0f, 0.001f, "Envelope"),
            makeParam("gain", "Gain", 1.0f, 0.0f, 8.0f, 0.01f, "Output"),
            makeParam("offset", "Offset", 0.0f, -1.0f, 1.0f, 0.001f, "Output"),
            makeParam("floor", "Floor", 0.0f, 0.0f, 1.0f, 0.001f, "Output"),
        };
        def.evaluate = evalEnvelope;
        add(std::move(def));
    }
    {
        NodeDef def;
        def.kind = "dsp.remap";
        def.category = "DSP";
        def.label = "Curve / Remap";
        def.description = "Remaps an input range onto an output range with optional shaping.";
        def.inputs = {PortDesc{"Value", PortType::Scalar}};
        def.outputs = {PortDesc{"Value", PortType::Scalar}};
        def.params = {
            makeParam("inMin", "In min", 0.0f, -16.0f, 16.0f, 0.01f, "Input range"),
            makeParam("inMax", "In max", 1.0f, -16.0f, 16.0f, 0.01f, "Input range"),
            makeParam("outMin", "Out min", 0.0f, -16.0f, 16.0f, 0.01f, "Output range"),
            makeParam("outMax", "Out max", 1.0f, -16.0f, 16.0f, 0.01f, "Output range"),
            makeParam("power", "Power", 1.0f, 0.05f, 6.0f, 0.01f, "Shape"),
            makeBoolParam("smoothstep", "Smoothstep", false, "Shape"),
            makeBoolParam("clamp", "Clamp output", true, "Shape"),
        };
        def.evaluate = evalRemap;
        add(std::move(def));
    }
    {
        NodeDef def;
        def.kind = "dsp.dynamics";
        def.category = "DSP";
        def.label = "Dynamics";
        def.description =
            "Zero-latency single-band compressor / downward expander with an optional 0 dBFS "
            "output limiter (hard or soft clip). The block draws dry and wet loudness in dBFS; "
            "Pre-gain, Threshold, Ratio, Attack, Release and Post-gain accept modulation.";
        def.inputs = {
            PortDesc{"Audio", PortType::Audio},
            PortDesc{"Pre-gain", PortType::Scalar, "gain x (1 + input)"},
            PortDesc{"Threshold", PortType::Scalar, "adds 24 dB per unit"},
            PortDesc{"Ratio", PortType::Scalar, "ratio x (1 + input)"},
            PortDesc{"Attack", PortType::Scalar, "time x 2^input"},
            PortDesc{"Release", PortType::Scalar, "time x 2^input"},
            PortDesc{"Post-gain", PortType::Scalar, "gain x (1 + input)"},
        };
        def.outputs = {PortDesc{"Audio", PortType::Audio, "processed clip"}};
        Param preGain = makeParam("preGain", "Pre-gain", 0.0f, -24.0f, 24.0f, 0.1f, "Gain");
        Param postGain = makeParam("postGain", "Post-gain", 0.0f, -24.0f, 24.0f, 0.1f, "Gain");
        Param threshold =
            makeParam("threshold", "Trigger (dBFS)", -18.0f, -60.0f, 0.0f, 0.1f, "Dynamics");
        Param ratio = makeParam("ratio", "Ratio", 4.0f, 1.0f, 20.0f, 0.1f, "Dynamics");
        Param attack = makeParam("attack", "Attack (ms)", 10.0f, 0.1f, 200.0f, 0.1f, "Dynamics");
        Param release =
            makeParam("release", "Release (ms)", 150.0f, 5.0f, 2000.0f, 1.0f, "Dynamics");
        Param limiterAttack =
            makeParam("limiterAttack", "Attack (ms)", 1.0f, 0.1f, 100.0f, 0.1f, "Limiter");
        Param limiterRelease =
            makeParam("limiterRelease", "Release (ms)", 80.0f, 5.0f, 1000.0f, 1.0f, "Limiter");
        preGain.valueFormat = "%.1f dB";
        postGain.valueFormat = "%.1f dB";
        threshold.valueFormat = "%.1f dBFS";
        ratio.valueFormat = "%.1f : 1";
        attack.valueFormat = "%.1f ms";
        release.valueFormat = "%.0f ms";
        limiterAttack.valueFormat = "%.1f ms";
        limiterRelease.valueFormat = "%.0f ms";
        def.params = {
            makeEnumParam("mode", "Mode", {"Compress", "Expand"}, 0, "Dynamics"),
            preGain,
            postGain,
            threshold,
            ratio,
            attack,
            release,
            makeBoolParam("limiter", "Limiter (0 dBFS)", true, "Limiter"),
            makeEnumParam("limiterMode", "Clip", {"Hard Clip", "Soft Clip"}, 1, "Limiter"),
            limiterAttack,
            limiterRelease,
        };
        def.evaluate = evalDynamics;
        add(std::move(def));
    }
    {
        NodeDef def;
        def.kind = "dsp.adc";
        def.category = "DSP";
        def.label = "ADC";
        def.description =
            "Audio to Scalar. On a path to a DAC the Scalar is the waveform at the audio "
            "sample rate, so every Math/Modulation block between them processes every sample. "
            "Channels are optional Scalar ports (left, right, ...; default 2) for stereo. "
            "Without a DAC downstream, Unity passes the waveform through and RMS/Peak follow "
            "loudness at video rate.";
        def.inputs = {PortDesc{"Audio", PortType::Audio}};
        def.outputs = {PortDesc{"left", PortType::Scalar, "sample"}};
        def.params = {
            makeEnumParam("mode", "Mode", {"Unity", "RMS", "Peak"}, 0, "Conversion"),
            makeIntParam("channels", "Channels", 2, 1, 8, "Conversion"),
        };
        def.evaluate = evalAdc;
        add(std::move(def));
    }
    {
        NodeDef def;
        def.kind = "dsp.dac";
        def.category = "DSP";
        def.label = "DAC";
        def.description =
            "Scalar to Audio. Ends an ADC -> DAC region: the per-sample Scalar stream becomes "
            "audio at the incoming rate, so the blocks before it processed the waveform "
            "itself. Channels are optional Scalar ports (left, right, ...; default 2) for "
            "stereo. Without an ADC upstream the per-frame value becomes the sample. Patch the "
            "output into an Audio Output to hear it.";
        def.inputs = {PortDesc{"left", PortType::Scalar, "sample"}};
        def.outputs = {PortDesc{"Audio", PortType::Audio, "rendered clip"}};
        def.params = {
            makeBoolParam("interpolate", "Interpolate frames", true, "Conversion"),
            makeBoolParam("clamp", "Clamp to -1..1", true, "Conversion"),
            makeIntParam("channels", "Channels", 2, 1, 8, "Conversion"),
        };
        def.evaluate = evalDac;
        add(std::move(def));
    }

    // ---- Math ------------------------------------------------------------
    {
        NodeDef def;
        def.kind = "math.constant";
        def.category = "Math";
        def.label = "Constant";
        def.description = "A fixed scalar value, handy for trims, thresholds and scaling.";
        def.outputs = {PortDesc{"Value", PortType::Scalar}};
        def.params = {makeParam("value", "Value", 1.0f, -1000.0f, 1000.0f, 0.0f, "Value")};
        def.evaluate = evalConstant;
        add(std::move(def));
    }
    {
        NodeDef def;
        def.kind = "math.arithmetic";
        def.category = "Math";
        def.label = "Arithmetic";
        def.description =
            "Add, subtract, multiply, divide, min, max or modulo. Input B falls back to the "
            "constant when unconnected.";
        def.inputs = {PortDesc{"A", PortType::Scalar}, PortDesc{"B", PortType::Scalar}};
        def.outputs = {PortDesc{"Result", PortType::Scalar}};
        def.params = {
            makeEnumParam("op", "Operation",
                          {"Add", "Subtract", "Multiply", "Divide", "Min", "Max", "Modulo"}, 0,
                          "Operation"),
            makeParam("bValue", "B (constant)", 0.0f, -64.0f, 64.0f, 0.0f, "Operation"),
            makeParam("gain", "Gain", 1.0f, -16.0f, 16.0f, 0.01f, "Output"),
            makeParam("offset", "Offset", 0.0f, -16.0f, 16.0f, 0.01f, "Output"),
            makeParam("clampMin", "Clamp min", -64.0f, -1.0e6f, 1.0e6f, 0.0f, "Output"),
            makeParam("clampMax", "Clamp max", 64.0f, -1.0e6f, 1.0e6f, 0.0f, "Output"),
        };
        def.evaluate = evalArithmetic;
        add(std::move(def));
    }
    {
        NodeDef def;
        def.kind = "math.power";
        def.category = "Math";
        def.label = "Power";
        def.description =
            "Base raised to an exponent. Negative bases keep their sign so the result stays "
            "usable as a signal - it never turns into NaN.";
        def.inputs = {PortDesc{"Base", PortType::Scalar}, PortDesc{"Exponent", PortType::Scalar}};
        def.outputs = {PortDesc{"Result", PortType::Scalar}};
        def.params = {
            makeParam("exponent", "Exponent (constant)", 2.0f, -16.0f, 16.0f, 0.0f, "Power"),
            makeBoolParam("signed", "Keep base sign", true, "Power"),
            makeParam("gain", "Gain", 1.0f, -16.0f, 16.0f, 0.01f, "Output"),
            makeParam("offset", "Offset", 0.0f, -16.0f, 16.0f, 0.01f, "Output"),
            makeParam("clampMin", "Clamp min", -64.0f, -1.0e6f, 1.0e6f, 0.0f, "Output"),
            makeParam("clampMax", "Clamp max", 64.0f, -1.0e6f, 1.0e6f, 0.0f, "Output"),
        };
        def.evaluate = evalPower;
        add(std::move(def));
    }
    {
        NodeDef def;
        def.kind = "math.exp";
        def.category = "Math";
        def.label = "Exponential";
        def.description = "e^x, 2^x or 10^x with input scaling and output gain/offset.";
        def.inputs = {PortDesc{"X", PortType::Scalar}};
        def.outputs = {PortDesc{"Result", PortType::Scalar}};
        def.params = {
            makeEnumParam("base", "Base", {"e", "2", "10"}, 0, "Function"),
            makeParam("inputScale", "Input scale", 1.0f, -16.0f, 16.0f, 0.01f, "Function"),
            makeParam("gain", "Gain", 1.0f, -16.0f, 16.0f, 0.01f, "Output"),
            makeParam("offset", "Offset", 0.0f, -16.0f, 16.0f, 0.01f, "Output"),
            makeParam("clampMin", "Clamp min", -64.0f, -1.0e6f, 1.0e6f, 0.0f, "Output"),
            makeParam("clampMax", "Clamp max", 64.0f, -1.0e6f, 1.0e6f, 0.0f, "Output"),
        };
        def.evaluate = evalExponential;
        add(std::move(def));
    }
    {
        NodeDef def;
        def.kind = "math.log";
        def.category = "Math";
        def.label = "Logarithm";
        def.description =
            "Natural, base-2 or base-10 logarithm. Inputs below the floor are clamped so the "
            "output stays finite.";
        def.inputs = {PortDesc{"X", PortType::Scalar}};
        def.outputs = {PortDesc{"Result", PortType::Scalar}};
        def.params = {
            makeEnumParam("base", "Base", {"ln", "log2", "log10"}, 0, "Function"),
            makeParam("floor", "Input floor", 1.0e-4f, 1.0e-9f, 1.0f, 0.0f, "Function"),
            makeParam("gain", "Gain", 1.0f, -16.0f, 16.0f, 0.01f, "Output"),
            makeParam("offset", "Offset", 0.0f, -16.0f, 16.0f, 0.01f, "Output"),
            makeParam("clampMin", "Clamp min", -64.0f, -1.0e6f, 1.0e6f, 0.0f, "Output"),
            makeParam("clampMax", "Clamp max", 64.0f, -1.0e6f, 1.0e6f, 0.0f, "Output"),
        };
        def.evaluate = evalLogarithm;
        add(std::move(def));
    }
    {
        NodeDef def;
        def.kind = "math.trig";
        def.category = "Math";
        def.label = "Trigonometry";
        def.description = "Sine, cosine and tangent of one input, with frequency, phase and units.";
        def.inputs = {PortDesc{"X", PortType::Scalar}};
        def.outputs = {PortDesc{"Sin", PortType::Scalar}, PortDesc{"Cos", PortType::Scalar},
                       PortDesc{"Tan", PortType::Scalar}};
        def.params = {
            makeEnumParam("unit", "Input unit", {"Radians", "Turns", "Degrees"}, 1, "Angle"),
            makeParam("frequency", "Frequency", 1.0f, -64.0f, 64.0f, 0.0f, "Angle"),
            makeParam("phase", "Phase", 0.0f, -16.0f, 16.0f, 0.0f, "Angle"),
            makeParam("gain", "Gain", 1.0f, -16.0f, 16.0f, 0.01f, "Output"),
            makeParam("offset", "Offset", 0.0f, -16.0f, 16.0f, 0.01f, "Output"),
        };
        def.evaluate = evalTrig;
        add(std::move(def));
    }
    {
        NodeDef def;
        def.kind = "math.hyperbolic";
        def.category = "Math";
        def.label = "Hyperbolic";
        def.description = "sinh, cosh and tanh of one input. tanh is a soft clipper.";
        def.inputs = {PortDesc{"X", PortType::Scalar}};
        def.outputs = {PortDesc{"Sinh", PortType::Scalar}, PortDesc{"Cosh", PortType::Scalar},
                       PortDesc{"Tanh", PortType::Scalar}};
        def.params = {
            makeParam("inputScale", "Input scale", 1.0f, -16.0f, 16.0f, 0.0f, "Function"),
            makeParam("gain", "Gain", 1.0f, -16.0f, 16.0f, 0.01f, "Output"),
            makeParam("offset", "Offset", 0.0f, -16.0f, 16.0f, 0.01f, "Output"),
        };
        def.evaluate = evalHyperbolic;
        add(std::move(def));
    }
    {
        NodeDef def;
        def.kind = "math.inverse_trig";
        def.category = "Math";
        def.label = "Inverse Trig";
        def.description =
            "arcsin, arccos and arctan. The arcsin/arccos input is clamped to -1..1 and the "
            "result can be returned in radians, turns or degrees.";
        def.inputs = {PortDesc{"X", PortType::Scalar}};
        def.outputs = {PortDesc{"Asin", PortType::Scalar}, PortDesc{"Acos", PortType::Scalar},
                       PortDesc{"Atan", PortType::Scalar}};
        def.params = {
            makeEnumParam("unit", "Output unit", {"Radians", "Turns", "Degrees"}, 0, "Angle"),
            makeParam("gain", "Gain", 1.0f, -16.0f, 16.0f, 0.01f, "Output"),
            makeParam("offset", "Offset", 0.0f, -16.0f, 16.0f, 0.01f, "Output"),
        };
        def.evaluate = evalInverseTrig;
        add(std::move(def));
    }
    {
        NodeDef def;
        def.kind = "math.vec2";
        def.category = "Math";
        def.label = "Vector2";
        def.description =
            "Builds a Vector2 from two scalars and also exposes the components as scalars.";
        def.inputs = {PortDesc{"X", PortType::Scalar}, PortDesc{"Y", PortType::Scalar}};
        def.outputs = {PortDesc{"Vector2", PortType::Vec2}, PortDesc{"X", PortType::Scalar},
                       PortDesc{"Y", PortType::Scalar}};
        def.params = {
            makeParam("x", "Default X", 0.0f, -16.0f, 16.0f, 0.0f, "Defaults"),
            makeParam("y", "Default Y", 0.0f, -16.0f, 16.0f, 0.0f, "Defaults"),
        };
        def.evaluate = evalVector2;
        add(std::move(def));
    }
    {
        NodeDef def;
        def.kind = "math.vec3";
        def.category = "Math";
        def.label = "Vector3";
        def.description = "Builds a Vector3, for example an RGB or XYZ triple.";
        def.inputs = {PortDesc{"X", PortType::Scalar}, PortDesc{"Y", PortType::Scalar},
                      PortDesc{"Z", PortType::Scalar}};
        def.outputs = {PortDesc{"Vector3", PortType::Vec3}, PortDesc{"X", PortType::Scalar},
                       PortDesc{"Y", PortType::Scalar}, PortDesc{"Z", PortType::Scalar}};
        def.params = {
            makeParam("x", "Default X", 0.0f, -16.0f, 16.0f, 0.0f, "Defaults"),
            makeParam("y", "Default Y", 0.0f, -16.0f, 16.0f, 0.0f, "Defaults"),
            makeParam("z", "Default Z", 0.0f, -16.0f, 16.0f, 0.0f, "Defaults"),
        };
        def.evaluate = evalVector3;
        add(std::move(def));
    }
    {
        NodeDef def;
        def.kind = "math.vec4";
        def.category = "Math";
        def.label = "Vector4";
        def.description = "Builds a Vector4, for example an RGBA colour or a matrix row.";
        def.inputs = {PortDesc{"X", PortType::Scalar}, PortDesc{"Y", PortType::Scalar},
                      PortDesc{"Z", PortType::Scalar}, PortDesc{"W", PortType::Scalar}};
        def.outputs = {PortDesc{"Vector4", PortType::Vec4}, PortDesc{"X", PortType::Scalar},
                       PortDesc{"Y", PortType::Scalar}, PortDesc{"Z", PortType::Scalar},
                       PortDesc{"W", PortType::Scalar}};
        def.params = {
            makeParam("x", "Default X", 0.0f, -16.0f, 16.0f, 0.0f, "Defaults"),
            makeParam("y", "Default Y", 0.0f, -16.0f, 16.0f, 0.0f, "Defaults"),
            makeParam("z", "Default Z", 0.0f, -16.0f, 16.0f, 0.0f, "Defaults"),
            makeParam("w", "Default W", 0.0f, -16.0f, 16.0f, 0.0f, "Defaults"),
        };
        def.evaluate = evalVector4;
        add(std::move(def));
    }
    {
        NodeDef def;
        def.kind = "math.matrix";
        def.category = "Math";
        def.label = "Matrix";
        def.description =
            "Builds a 2x2, 3x3 or 4x4 matrix from up to four Vector4 rows. Unconnected rows "
            "fall back to the identity row they show.";
        def.inputs = {PortDesc{"Row 0", PortType::Vec4}, PortDesc{"Row 1", PortType::Vec4},
                      PortDesc{"Row 2", PortType::Vec4}, PortDesc{"Row 3", PortType::Vec4}};
        def.outputs = {PortDesc{"Matrix", PortType::Matrix}};
        def.params = {
            makeEnumParam("size", "Size", {"2x2", "3x3", "4x4"}, 1, "Size"),
            makeMatrixParam("matrix", "Matrix", 4),
        };
        def.evaluate = evalMatrix;
        add(std::move(def));
    }
    {
        NodeDef def;
        def.kind = "math.determinant";
        def.category = "Math";
        def.label = "Determinant";
        def.description =
            "Determinant and trace of the top-left 2x2, 3x3 or 4x4 block of a matrix.";
        def.inputs = {PortDesc{"Matrix", PortType::Matrix}};
        def.outputs = {PortDesc{"Determinant", PortType::Scalar},
                       PortDesc{"Trace", PortType::Scalar}};
        def.params = {makeEnumParam("size", "Size", {"2x2", "3x3", "4x4"}, 1, "Size")};
        def.evaluate = evalDeterminant;
        add(std::move(def));
    }
    {
        // Noise takes no Audio/Analysis input, so it belongs with the Math
        // generators even though it started in DSP.
        NodeDef def;
        def.kind = "dsp.noise";
        def.category = "Math";
        def.label = "Noise";
        def.description = "Value, sample-and-hold or pink-ish noise as a 0..1 control signal.";
        def.outputs = {PortDesc{"Value", PortType::Scalar}};
        def.params = {
            makeEnumParam("type", "Type", {"Value noise", "Sample & hold", "Pink"}, 0, "Noise"),
            makeParam("speed", "Speed", 1.0f, 0.01f, 40.0f, 0.01f, "Noise"),
            makeParam("amount", "Amount", 1.0f, 0.0f, 1.0f, 0.01f, "Noise"),
            makeIntParam("seed", "Seed", 1, 1, 9999, "Noise"),
        };
        def.evaluate = evalNoise;
        add(std::move(def));
    }

    // ---- Modulation ------------------------------------------------------
    {
        NodeDef def;
        def.kind = "mod.lfo";
        def.category = "Modulation";
        def.label = "LFO";
        def.description =
            "Low frequency oscillator, optionally locked to a musical division. Frequency, "
            "amplitude and offset can be modulated (the frequency input shifts by octaves).";
        def.inputs = {PortDesc{"Phase", PortType::Scalar, "optional phase offset"},
                      PortDesc{"Frequency", PortType::Scalar, "Hz x 2^input"},
                      PortDesc{"Amplitude", PortType::Scalar, "scales the amplitude"},
                      PortDesc{"Offset", PortType::Scalar, "added to the output"}};
        def.outputs = {PortDesc{"Value", PortType::Scalar}};
        def.params = {
            makeEnumParam("shape", "Shape",
                          {"Sine", "Triangle", "Saw up", "Saw down", "Square", "Random"}, 0,
                          "Shape"),
            makeParam("frequency", "Frequency (Hz)", 0.5f, 0.0f, 40.0f, 0.001f, "Shape"),
            makeParam("phase", "Phase", 0.0f, -1.0f, 1.0f, 0.001f, "Shape"),
            makeParam("amplitude", "Amplitude", 1.0f, -4.0f, 4.0f, 0.01f, "Output"),
            makeParam("offset", "Offset", 0.0f, -4.0f, 4.0f, 0.01f, "Output"),
            makeBoolParam("sync", "Sync to tempo", false, "Tempo"),
            makeParam("bpm", "Tempo (BPM)", 120.0f, 20.0f, 300.0f, 1.0f, "Tempo"),
            makeEnumParam("division", "Division", {"1/1", "1/2", "1/4", "1/8", "1/16"}, 1, "Tempo"),
        };
        def.evaluate = evalLfo;
        add(std::move(def));
    }
    {
        NodeDef def;
        def.kind = "mod.automation";
        def.category = "Modulation";
        def.label = "Automation";
        def.description =
            "Keyframed curve over the project timeline, edited here in the block. Unipolar maps "
            "the curve to 0..1, bipolar to -1..1. Depth and offset can be modulated.";
        def.inputs = {PortDesc{"Depth", PortType::Scalar, "scales the depth"},
                      PortDesc{"Offset", PortType::Scalar, "added to the output"}};
        def.outputs = {PortDesc{"Value", PortType::Scalar}};
        Param curve = makeCurveParam("curve", "Curve");
        def.params = {
            curve,
            makeBoolParam("bipolar", "Bipolar (-1..1)", false, "Range"),
            makeParam("depth", "Depth", 1.0f, 0.0f, 4.0f, 0.01f, "Range"),
            makeParam("offset", "Offset", 0.0f, -4.0f, 4.0f, 0.01f, "Range"),
            makeBoolParam("loop", "Loop", false, "Output"),
            makeParam("loopLength", "Loop length (x timeline)", 1.0f, 0.05f, 1.0f, 0.01f, "Output"),
            makeBoolParam("smooth", "Smooth", false, "Output"),
            makeParam("smoothing", "Smoothing", 0.2f, 0.001f, 1.0f, 0.001f, "Output"),
        };
        def.evaluate = evalAutomation;
        add(std::move(def));
    }
    {
        NodeDef def;
        def.kind = "mod.amount";
        def.category = "Modulation";
        def.label = "Amount / VCA";
        def.description =
            "Scales a signal by a constant or by a second modulation input. Amount, gain and "
            "offset can all be modulated.";
        def.inputs = {PortDesc{"In", PortType::Scalar}, PortDesc{"Amount", PortType::Scalar},
                      PortDesc{"Gain", PortType::Scalar, "scales the gain"},
                      PortDesc{"Offset", PortType::Scalar, "added to the output"}};
        def.outputs = {PortDesc{"Out", PortType::Scalar}};
        def.params = {
            makeParam("amount", "Amount", 1.0f, 0.0f, 4.0f, 0.001f, "Amount"),
            makeParam("gain", "Gain", 1.0f, -8.0f, 8.0f, 0.01f, "Amount"),
            makeParam("offset", "Offset", 0.0f, -8.0f, 8.0f, 0.01f, "Amount"),
            makeParam("curve", "Curve", 1.0f, 0.05f, 4.0f, 0.01f, "Shape"),
            makeIntParam("quantize", "Quantise steps (0 = off)", 0, 0, 64, "Shape"),
        };
        def.evaluate = evalAmount;
        add(std::move(def));
    }
    {
        NodeDef def;
        def.kind = "mod.ringbuffer";
        def.category = "Modulation";
        def.label = "Ringbuffer";
        def.description =
            "Records the incoming Scalar into a loop buffer. Outputs the live input, the running "
            "average of the buffer, and the value the read pointer is currently passing over. The "
            "loop speed can be modulated in octaves.";
        def.inputs = {PortDesc{"In", PortType::Scalar},
                      PortDesc{"Speed", PortType::Scalar, "loops/s x 2^input"}};
        def.outputs = {PortDesc{"Input", PortType::Scalar},
                       PortDesc{"Average", PortType::Scalar},
                       PortDesc{"Buffer", PortType::Scalar}};
        def.params = {
            makeIntParam("size", "Size (samples)", 64, 2, 1024, "Buffer"),
            makeParam("speed", "Loop speed (loops/s)", 0.5f, 0.01f, 8.0f, 0.01f, "Buffer"),
        };
        def.evaluate = evalRingbuffer;
        add(std::move(def));
    }
    {
        NodeDef def;
        def.kind = "mod.filter";
        def.category = "Modulation";
        def.label = "Signal Filter";
        def.description =
            "Zero-latency biquad (RBJ, transposed direct form II): low pass, high pass or "
            "band pass. Outside an ADC -> DAC region it filters modulation at the video frame "
            "rate; inside one it filters every audio sample at the audio rate. Cutoff and "
            "resonance can be modulated in octaves.";
        def.inputs = {PortDesc{"In", PortType::Scalar},
                      PortDesc{"Cutoff", PortType::Scalar, "Hz x 2^input"},
                      PortDesc{"Resonance", PortType::Scalar, "Q x 2^input"}};
        def.outputs = {PortDesc{"Out", PortType::Scalar}};
        Param cutoff = makeParam("cutoff", "Cutoff (Hz)", 4.0f, 0.01f, 20000.0f, 0.01f, "Filter",
                                 true);
        cutoff.valueFormat = "%.2f Hz";
        def.params = {
            makeEnumParam("mode", "Mode", {"Low pass", "High pass", "Band pass"}, 0, "Filter"),
            cutoff,
            makeParam("resonance", "Resonance (Q)", 0.707f, 0.05f, 20.0f, 0.001f, "Filter"),
        };
        def.evaluate = evalSignalFilter;
        add(std::move(def));
    }

    // ---- Render ----------------------------------------------------------
    {
        NodeDef def;
        def.kind = "render.spectrum";
        def.category = "Render";
        def.label = "Spectrum";
        def.description =
            "Draws the spectrum, waveform or spectrogram of an Analysis input as an Image "
            "with one of the built-in effects. The Scale, Feedback and Colour inputs "
            "modulate the matching parameters, so any modulation block can drive them.";
        def.inputs = {PortDesc{"Analysis", PortType::Analysis, "spectrum source"},
                      PortDesc{"Scale", PortType::Scalar, "modulates Resolution scale"},
                      PortDesc{"Feedback", PortType::Scalar, "modulates Feedback amount"},
                      PortDesc{"Colour A", PortType::Color, "overrides Colour A"},
                      PortDesc{"Colour B", PortType::Color, "overrides Colour B"}};
        def.outputs = {PortDesc{"Image", PortType::Image}};
        std::vector<std::string> builtinOptions = ShaderLibrary::effectNames();
        std::vector<Param> params;
        params.push_back(makeEnumParam("preset", "Built-in effect", builtinOptions, 0, "Effect"));
        params.push_back(colorParam("colorA", "Colour A", 0x3A6BFF, "Look"));
        params.push_back(colorParam("colorB", "Colour B", 0xFF4FA3, "Look"));
        params.push_back(makeParam("scale", "Resolution scale", 1.0f, 0.15f, 2.0f, 0.05f, "Look"));
        params.push_back(makeBoolParam("useFeedback", "Feedback", false, "Look"));
        params.push_back(makeParam("feedback", "Feedback amount", 0.6f, 0.0f, 0.98f, 0.01f, "Look"));
        def.params = std::move(params);
        def.evaluate = evalSpectrum;
        add(std::move(def));
    }
    {
        NodeDef def;
        def.kind = "render.shader";
        def.category = "Render";
        def.label = "Shader";
        def.description =
            "Applies a .glsl fragment shader to the incoming Image. The input ports follow "
            "the uniforms the file actually uses: uPrev, uInput2, uUser[0..7], uColorA/B, "
            "uVector2/3/4 and uMatrix. The PulseForge preamble is prepended automatically.";
        def.inputs = {PortDesc{"uPrev", PortType::Image, "upstream image"}};
        def.outputs = {PortDesc{"Image", PortType::Image}};
        def.params = {
            makeFileParam("shader", "Shader file", "", ".glsl", "Shader"),
            colorParam("colorA", "Colour A", 0x3A6BFF, "Look"),
            colorParam("colorB", "Colour B", 0xFF4FA3, "Look"),
            makeParam("scale", "Resolution scale", 1.0f, 0.15f, 2.0f, 0.05f, "Look"),
            makeBoolParam("useFeedback", "Feedback", false, "Look"),
        };
        def.evaluate = evalShader;
        add(std::move(def));
    }
    {
        NodeDef def;
        def.kind = "fx.blend";
        def.category = "Render";
        def.label = "Blend";
        def.description = "Composites two images with the selected blend mode.";
        def.inputs = {PortDesc{"A", PortType::Image}, PortDesc{"B", PortType::Image}};
        def.outputs = {PortDesc{"Image", PortType::Image}};
        def.params = {
            makeEnumParam("mode", "Mode",
                          {"Cross fade", "Add", "Screen", "Multiply", "Difference", "Overlay",
                           "Min", "Max"},
                          1, "Blend"),
            makeParam("opacity", "Opacity", 1.0f, 0.0f, 1.0f, 0.01f, "Blend"),
        };
        def.evaluate = evalBlend;
        add(std::move(def));
    }
    {
        NodeDef def;
        def.kind = "fx.postfx";
        def.category = "Render";
        def.label = "Post FX";
        def.description =
            "Bloom, chromatic aberration, vignette, grain, scanlines, feedback and grade. "
            "Scalar inputs override the matching parameter, so modulation blocks can drive it.";
        def.inputs = {PortDesc{"Image", PortType::Image},
                      PortDesc{"Bloom", PortType::Scalar},
                      PortDesc{"Chromatic", PortType::Scalar},
                      PortDesc{"Vignette", PortType::Scalar},
                      PortDesc{"Grain", PortType::Scalar},
                      PortDesc{"Scanlines", PortType::Scalar},
                      PortDesc{"Feedback", PortType::Scalar},
                      PortDesc{"Saturation", PortType::Scalar},
                      PortDesc{"Hue", PortType::Scalar}};
        def.outputs = {PortDesc{"Image", PortType::Image}};
        def.params = {
            makeParam("bloom", "Bloom", 0.35f, 0.0f, 1.0f, 0.01f, "Glow"),
            makeParam("chromatic", "Chromatic", 0.15f, 0.0f, 1.0f, 0.01f, "Glow"),
            makeParam("feedback", "Feedback", 0.0f, 0.0f, 0.98f, 0.01f, "Glow"),
            makeParam("vignette", "Vignette", 0.35f, 0.0f, 1.0f, 0.01f, "Grade"),
            makeParam("saturation", "Saturation", 1.05f, 0.0f, 2.0f, 0.01f, "Grade"),
            makeParam("hue", "Hue shift", 0.0f, -1.0f, 1.0f, 0.01f, "Grade"),
            makeParam("grain", "Grain", 0.08f, 0.0f, 1.0f, 0.01f, "Texture"),
            makeParam("scanlines", "Scanlines", 0.0f, 0.0f, 1.0f, 0.01f, "Texture"),
        };
        def.evaluate = evalPostFx;
        add(std::move(def));
    }
    {
        NodeDef def;
        def.kind = "geom.primitives";
        def.category = "Render";
        def.label = "Geometry";
        def.description =
            "Draws geometric primitives (circle, ring, polygon grid, sparks, orbit, text) "
            "on top of an optional image layer. Scale, rotation and position can be driven "
            "by scalar/vector inputs; spectrum and waveform visuals live in the Spectrum "
            "block instead.";
        def.inputs = {PortDesc{"Layer", PortType::Image, "optional background"},
                      PortDesc{"Scale", PortType::Scalar},
                      PortDesc{"Rotation", PortType::Scalar},
                      PortDesc{"X", PortType::Scalar},
                      PortDesc{"Y", PortType::Scalar},
                      PortDesc{"Position", PortType::Vec2, "overrides X/Y"}};
        def.outputs = {PortDesc{"Image", PortType::Image}};
        def.params = {
            makeEnumParam("shape", "Shape", geometry::shapeNames(), 0, "Shape"),
            makeIntParam("count", "Count", 64, 2, 512, "Shape"),
            makeParam("radius", "Radius", 0.28f, 0.01f, 1.5f, 0.005f, "Shape"),
            makeParam("thickness", "Thickness", 4.0f, 0.5f, 40.0f, 0.5f, "Shape"),
            makeParam("x", "Position X", 0.5f, -1.0f, 2.0f, 0.005f, "Transform"),
            makeParam("y", "Position Y", 0.5f, -1.0f, 2.0f, 0.005f, "Transform"),
            makeParam("rotation", "Rotation (turns)", 0.0f, -2.0f, 2.0f, 0.005f, "Transform"),
            makeParam("spin", "Spin (turns/s)", 0.0f, -4.0f, 4.0f, 0.005f, "Transform"),
            makeParam("scaleMod", "Scale mod", 0.5f, -4.0f, 4.0f, 0.01f, "Modulation"),
            makeParam("rotationMod", "Rotation mod", 0.0f, -4.0f, 4.0f, 0.01f, "Modulation"),
            makeParam("xMod", "X mod", 0.25f, -4.0f, 4.0f, 0.01f, "Modulation"),
            makeParam("yMod", "Y mod", 0.0f, -4.0f, 4.0f, 0.01f, "Modulation"),
            colorParam("colorA", "Colour A", 0x59B2FF, "Look"),
            colorParam("colorB", "Colour B", 0xFF7BD1, "Look"),
            makeParam("alpha", "Opacity", 1.0f, 0.0f, 1.0f, 0.01f, "Look"),
            makeBoolParam("additive", "Additive blend", true, "Look"),
            makeTextParam("text", "Text", "PulseForge", "Text"),
            makeParam("textSize", "Text size", 72.0f, 8.0f, 400.0f, 1.0f, "Text"),
        };
        def.evaluate = evalGeometry;
        add(std::move(def));
    }

    // ---- Output ----------------------------------------------------------
    {
        NodeDef def;
        def.kind = "out.video";
        def.category = "Output";
        def.label = "Video Output";
        def.description = "Terminal block: whatever is connected here is rendered and exported.";
        def.inputs = {PortDesc{"Image", PortType::Image}};
        def.params = {};
        def.evaluate = evalOutput;
        def.isSink = true;
        add(std::move(def));
    }
    {
        NodeDef def;
        def.kind = "out.audio";
        def.category = "Output";
        def.label = "Audio Output";
        def.description =
            "Terminal block: the Audio signal connected here is the only audio the export "
            "carries. Leave it disconnected for a silent video, or patch a DAC into it to "
            "synthesise the soundtrack from Scalars.";
        def.inputs = {PortDesc{"Audio", PortType::Audio}};
        def.outputs = {};
        def.params = {};
        def.evaluate = evalAudioOutput;
        def.isSink = true;
        add(std::move(def));
    }

    // ---- Debug -----------------------------------------------------------
    {
        NodeDef def;
        def.kind = "dbg.meter";
        def.category = "Debug";
        def.label = "VU / Digital Meter";
        def.description =
            "Passes a Scalar through untouched and shows it as a classic VU meter (0 VU = "
            "-18 dBFS) or as a value/time diagram. The needle has fast attack and slow release.";
        def.inputs = {PortDesc{"In", PortType::Scalar}};
        def.outputs = {PortDesc{"Out", PortType::Scalar}};
        def.params = {
            makeEnumParam("mode", "Display", {"VU meter", "Value graph"}, 0, "Display"),
            makeParam("ballistics", "Release", 0.55f, 0.0f, 0.95f, 0.01f, "Display"),
        };
        def.evaluate = evalMeter;
        add(std::move(def));
    }
    {
        NodeDef def;
        def.kind = "dbg.guard";
        def.category = "Debug";
        def.label = "Guard";
        def.description =
            "Silences non-finite scalars: NaN and +/-Inf inputs become 0. Three lamps show which "
            "kind of value was last seen.";
        def.inputs = {PortDesc{"In", PortType::Scalar}};
        def.outputs = {PortDesc{"Out", PortType::Scalar}};
        def.params = {};
        def.evaluate = evalGuard;
        add(std::move(def));
    }
}

}  // namespace pf
