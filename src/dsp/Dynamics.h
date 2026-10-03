// Zero-latency single-band dynamics: a compressor and a downward expander with
// an optional 0 dBFS output limiter. The processor is stateful because the flow
// graph evaluates one video frame at a time; callers keep the state between
// chunks so the detector envelopes stay continuous.
#pragma once

#include <vector>

#include "core/Port.h"

namespace pf {

struct DynamicsSettings {
    bool expand = false;             // false: compress above, true: expand below threshold
    float preGainDb = 0.0f;
    float thresholdDb = -18.0f;
    float ratio = 4.0f;              // >= 1
    float attackMs = 10.0f;
    float releaseMs = 150.0f;
    float postGainDb = 0.0f;
    bool limiter = true;
    bool softClip = true;            // false: hard clip
    float limiterAttackMs = 1.0f;
    float limiterReleaseMs = 80.0f;
};

// Envelope followers and limiter gain carried across chunks.
struct DynamicsState {
    // Linear peak envelope. Holding the peak between cycles (instead of
    // following the rectified waveform into every zero crossing) is what makes
    // the reduction read the same for a sine as for DC of the same peak.
    float detector = 0.0f;
    float gainDb = 0.0f;       // current gain reduction, <= 0
    float gain = 1.0f;         // linear-domain gain (1 = untouched)
    float limiterGain = 1.0f;  // 1 = untouched, < 1 = limiting

    void reset() {
        detector = 0.0f;
        gainDb = 0.0f;
        gain = 1.0f;
        limiterGain = 1.0f;
    }
};

// Gain reduction in dB (<= 0) the static curve asks for at a detector level.
float dynamicsGainDb(const DynamicsSettings &settings, float levelDb);

inline float dbToLinear(float db) { return std::pow(10.0f, db / 20.0f); }

// -120 dB floor keeps silence finite and the labels readable.
float linearToDb(float linear, float floorDb = -120.0f);

// Processes `frames` interleaved frames. `input` and `output` may alias.
void processDynamicsBlock(const float *input, float *output, long long frames, int channels,
                          int sampleRate, const DynamicsSettings &settings, DynamicsState &state);

// RMS loudness of a frame window in dBFS, used by the in-block dry/wet graph.
float loudnessDb(const AudioBuffer &buffer, long long startFrame, long long frames);

}  // namespace pf
