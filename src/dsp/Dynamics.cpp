#include "dsp/Dynamics.h"

#include <algorithm>
#include <cmath>

namespace pf {

namespace {

// One-pole coefficient: the fraction of the remaining distance covered per
// sample. Time is the 1/e (63%) point, which matches how attack/release are
// usually labelled.
float timeCoefficient(float milliseconds, double sampleRate) {
    const double seconds = std::max(0.01, static_cast<double>(milliseconds)) / 1000.0;
    const double samples = std::max(1.0, seconds * std::max(1.0, sampleRate));
    return static_cast<float>(std::clamp(1.0 - std::exp(-1.0 / samples), 0.0, 1.0));
}

// Transparent below the knee and asymptotic to the 0 dBFS ceiling above it, so
// soft clipping only touches material that is already over.
float softLimit(float value) {
    constexpr float knee = 0.7f;
    const float magnitude = std::fabs(value);
    if (magnitude <= knee) return value;
    const float over = (magnitude - knee) / (1.0f - knee);
    const float shaped = knee + (1.0f - knee) * std::tanh(over);
    return std::copysign(shaped, value);
}

}  // namespace

float dynamicsGainDb(const DynamicsSettings &settings, float levelDb) {
    const float ratio = std::clamp(settings.ratio, 1.0f, 100.0f);
    const float threshold = settings.thresholdDb;
    if (settings.expand) {
        // Downward expander: below the threshold the level is pushed further
        // down by (ratio - 1) dB per dB.
        if (levelDb >= threshold) return 0.0f;
        return (levelDb - threshold) * (ratio - 1.0f);
    }
    if (levelDb <= threshold) return 0.0f;
    return (levelDb - threshold) * (1.0f / ratio - 1.0f);
}

float linearToDb(float linear, float floorDb) {
    if (!(linear > 0.0f) || !std::isfinite(linear)) return floorDb;
    const float db = 20.0f * std::log10(linear);
    return std::max(db, floorDb);
}

void processDynamicsBlock(const float *input, float *output, long long frames, int channels,
                          int sampleRate, const DynamicsSettings &settings, DynamicsState &state) {
    if (!input || !output || frames <= 0) return;
    channels = std::max(1, channels);
    const double rate = sampleRate > 0 ? sampleRate : 48000.0;

    const float attack = timeCoefficient(settings.attackMs, rate);
    const float release = timeCoefficient(settings.releaseMs, rate);
    const float limiterAttack = timeCoefficient(settings.limiterAttackMs, rate);
    const float limiterRelease = timeCoefficient(settings.limiterReleaseMs, rate);
    const float preGain = dbToLinear(std::clamp(settings.preGainDb, -60.0f, 60.0f));
    const float postGain = dbToLinear(std::clamp(settings.postGainDb, -60.0f, 60.0f));
    const float preGainStart =
        settings.rampGains
            ? dbToLinear(std::clamp(settings.preGainDbStart, -60.0f, 60.0f))
            : preGain;
    const float postGainStart =
        settings.rampGains
            ? dbToLinear(std::clamp(settings.postGainDbStart, -60.0f, 60.0f))
            : postGain;
    const float preGainStep = preGain - preGainStart;
    const float postGainStep = postGain - postGainStart;
    // Linear-domain static curve: gain = detector^exponent * factor. This
    // replaces the per-sample log10 + pow pair with a single pow.
    const float ratio = std::clamp(settings.ratio, 1.0f, 100.0f);
    const float exponent = settings.expand ? (ratio - 1.0f) : (1.0f / ratio - 1.0f);
    const float thresholdDb = std::clamp(settings.thresholdDb, -96.0f, 24.0f);
    const float thresholdLinear = dbToLinear(thresholdDb);
    const float curveFactor = dbToLinear(-thresholdDb * exponent);

    std::vector<float> gained(static_cast<size_t>(channels));
    for (long long frame = 0; frame < frames; ++frame) {
        const float *source = input + frame * channels;
        // Ramp across the block and land exactly on the target, so the next
        // window starts where this one ended.
        const float position = frames > 1 ? static_cast<float>(frame + 1) /
                                                static_cast<float>(frames)
                                          : 1.0f;
        const float windowPreGain = preGainStart + preGainStep * position;
        const float windowPostGain = postGainStart + postGainStep * position;

        // Detector: the loudest channel, after pre-gain, peak detected. A peak
        // detector is what makes the block 0-latency and transparent until the
        // threshold, which is the usual behaviour of a channel compressor.
        float peak = 0.0f;
        for (int channel = 0; channel < channels; ++channel) {
            float sample = source[channel];
            if (!std::isfinite(sample)) sample = 0.0f;
            peak = std::max(peak, std::fabs(sample));
        }
        peak *= windowPreGain;
        const float detectorCoefficient = peak > state.detector ? attack : release;
        state.detector += detectorCoefficient * (peak - state.detector);

        // Attack when the reduction grows, release when it recovers.
        float targetGain = 1.0f;
        const bool inRange =
            settings.expand ? state.detector < thresholdLinear
                            : state.detector > thresholdLinear;
        if (inRange) {
            targetGain = std::pow(std::max(state.detector, 1.0e-9f), exponent) * curveFactor;
        }
        const float gainCoefficient = targetGain < state.gain ? attack : release;
        state.gain += gainCoefficient * (targetGain - state.gain);
        const float gain = state.gain * windowPreGain * windowPostGain;

        float outputPeak = 0.0f;
        for (int channel = 0; channel < channels; ++channel) {
            float sample = source[channel];
            if (!std::isfinite(sample)) sample = 0.0f;
            gained[static_cast<size_t>(channel)] = sample * gain;
            outputPeak = std::max(outputPeak, std::fabs(gained[static_cast<size_t>(channel)]));
        }

        if (settings.limiter) {
            const float targetGain = outputPeak > 1.0f ? 1.0f / outputPeak : 1.0f;
            const float coefficient = targetGain < state.limiterGain ? limiterAttack : limiterRelease;
            state.limiterGain += coefficient * (targetGain - state.limiterGain);
            state.limiterGain = std::clamp(state.limiterGain, 0.0f, 1.0f);
        } else {
            state.limiterGain = 1.0f;
        }

        float *destination = output + frame * channels;
        for (int channel = 0; channel < channels; ++channel) {
            float sample = gained[static_cast<size_t>(channel)];
            if (settings.limiter) {
                sample *= state.limiterGain;
                sample = settings.softClip ? softLimit(sample) : std::clamp(sample, -1.0f, 1.0f);
            }
            destination[channel] = sample;
        }
    }
    state.gainDb = linearToDb(state.gain);
}

float loudnessDb(const AudioBuffer &buffer, long long startFrame, long long frames) {
    const int channels = std::max(1, buffer.channels);
    const long long first = std::clamp<long long>(startFrame, 0, buffer.frameCount);
    const long long last = std::clamp<long long>(startFrame + frames, first, buffer.frameCount);
    if (last <= first || buffer.samples.empty()) return -120.0f;
    double sum = 0.0;
    for (long long frame = first; frame < last; ++frame) {
        const size_t base = static_cast<size_t>(frame) * static_cast<size_t>(channels);
        for (int channel = 0; channel < channels; ++channel) {
            const float sample = buffer.samples[base + static_cast<size_t>(channel)];
            if (std::isfinite(sample)) sum += static_cast<double>(sample) * sample;
        }
    }
    const double count = static_cast<double>(last - first) * channels;
    const float rms = static_cast<float>(std::sqrt(sum / std::max(1.0, count)));
    return std::clamp(linearToDb(rms), -120.0f, 20.0f);
}

}  // namespace pf
