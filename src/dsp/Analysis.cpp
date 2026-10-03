#include "dsp/Analysis.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <map>
#include <thread>

#include "dsp/Fft.h"
#include "raylib.h"

namespace pf {

namespace {

inline float magnitudeToDb(float magnitude) {
    return 20.0f * std::log10(std::max(magnitude, 1e-7f));
}

// Maps a dB value onto 0..1 with a -80 dB floor.
inline float dbToUnit(float db) {
    return std::clamp((db + 80.0f) / 80.0f, 0.0f, 1.0f);
}

// Log-spaced FFT bin edges shared by the whole-file and live analysers.
std::vector<int> logBinEdges(int count, int bins, float low, float high, float nyquist) {
    const float logLow = std::log(low);
    const float logHigh = std::log(high);
    std::vector<int> edges(static_cast<size_t>(count) + 1);
    for (int i = 0; i <= count; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(count);
        const float hz = std::exp(logLow + (logHigh - logLow) * t);
        const float normalized = std::clamp(hz / nyquist, 0.0f, 0.9999f);
        int bin = std::max(0, std::min(bins - 1, static_cast<int>(normalized * bins)));
        if (i > 0 && bin <= edges[static_cast<size_t>(i - 1)]) {
            bin = std::min(bins - 1, edges[static_cast<size_t>(i - 1)] + 1);
        }
        edges[static_cast<size_t>(i)] = bin;
    }
    return edges;
}

// One FFT per size for the live analyser, so the twiddle tables are not rebuilt
// every video frame.
Fft &sharedFft(int size) {
    static thread_local std::map<int, Fft> cache;
    auto it = cache.find(size);
    if (it == cache.end()) it = cache.emplace(size, Fft(size)).first;
    return it->second;
}

}  // namespace

AnalysisPtr analyzeAudio(const AudioBuffer &buffer, const AnalysisSettings &settings,
                         const std::function<void(float)> &progress, const AudioPtr &source) {
    auto result = std::make_shared<AnalysisData>();
    if (buffer.frameCount <= 0 || buffer.sampleRate <= 0) return result;

    const int fftSize = std::max(256, settings.fftSize);
    const int hop = std::max(32, settings.hopSize);
    const int bins = fftSize / 2;
    const int spectrumBins = std::max(16, settings.spectrumBins);
    const int bandCount = std::max(4, settings.bandCount);
    const int sampleRate = buffer.sampleRate;

    result->sampleRate = sampleRate;
    result->fftSize = fftSize;
    result->hopSize = hop;
    result->bandCount = bandCount;
    result->spectrumBins = spectrumBins;
    result->duration = buffer.duration();
    result->source = source;

    const long long total = buffer.frameCount;
    const long long frameCount = total > fftSize ? ((total - fftSize) / hop + 1) : 1;
    result->frames.resize(static_cast<size_t>(frameCount));
    result->spectrum.assign(static_cast<size_t>(frameCount) * static_cast<size_t>(spectrumBins), 0.0f);

    // Precompute the frequency mapping for the log-spaced bins and bands.
    const float nyquist = static_cast<float>(sampleRate) * 0.5f;
    const float low = std::max(10.0f, settings.lowFrequency);
    const float high = std::min(nyquist, std::max(low * 2.0f, settings.highFrequency));
    const std::vector<int> spectrumBinEdges =
        logBinEdges(spectrumBins, bins, low, high, nyquist);
    const std::vector<int> bandEdges = logBinEdges(bandCount, bins, low, high, nyquist);

    const unsigned int hardware = std::thread::hardware_concurrency();
    const int workerCount = static_cast<int>(std::max(1u, hardware));
    const int chunks = std::min<int>(static_cast<int>(frameCount), workerCount * 8);
    std::atomic<int> completed{0};
    std::vector<std::thread> workers;
    const double startTime = GetTime();

    auto worker = [&](int index) {
        Fft fft(fftSize);
        std::vector<float> window(static_cast<size_t>(fftSize));
        std::vector<float> power(static_cast<size_t>(bins));
        std::vector<float> previous(static_cast<size_t>(bins), 0.0f);
        std::vector<float> magnitudes(static_cast<size_t>(bins));
        for (long long f = index; f < frameCount; f += chunks) {
            const long long offset = f * hop;
            for (int i = 0; i < fftSize; ++i) {
                const long long source = offset + i;
                window[static_cast<size_t>(i)] =
                    source < total ? buffer.monoAt(static_cast<double>(source)) : 0.0f;
            }
            float sumSquares = 0.0f;
            for (int i = 0; i < fftSize; ++i) {
                sumSquares += window[static_cast<size_t>(i)] * window[static_cast<size_t>(i)];
            }
            const float rms = std::sqrt(sumSquares / static_cast<float>(fftSize));
            applyHannWindow(window.data(), fftSize);
            fft.magnitude(window.data(), magnitudes.data());

            AnalysisFrame &frame = result->frames[static_cast<size_t>(f)];
            frame.bands.assign(static_cast<size_t>(bandCount), 0.0f);
            frame.rms = rms;

            float flux = 0.0f;
            for (int b = 0; b < bins; ++b) {
                const float value = magnitudes[static_cast<size_t>(b)];
                power[static_cast<size_t>(b)] = value;
                const float delta = value - previous[static_cast<size_t>(b)];
                if (delta > 0.0f) flux += delta;
                previous[static_cast<size_t>(b)] = value;
            }
            frame.flux = flux / static_cast<float>(bins);

            float *row = result->spectrum.data() +
                         static_cast<size_t>(f) * static_cast<size_t>(spectrumBins);
            for (int s = 0; s < spectrumBins; ++s) {
                const int from = spectrumBinEdges[static_cast<size_t>(s)];
                const int to = std::max(from + 1, spectrumBinEdges[static_cast<size_t>(s + 1)]);
                float sum = 0.0f;
                for (int b = from; b < to && b < bins; ++b) sum += power[static_cast<size_t>(b)];
                const float mean = sum / static_cast<float>(std::max(1, to - from));
                row[s] = dbToUnit(magnitudeToDb(mean / static_cast<float>(fftSize)));
            }
            for (int band = 0; band < bandCount; ++band) {
                const int from = bandEdges[static_cast<size_t>(band)];
                const int to = std::max(from + 1, bandEdges[static_cast<size_t>(band + 1)]);
                float sum = 0.0f;
                for (int b = from; b < to && b < bins; ++b) sum += power[static_cast<size_t>(b)];
                const float mean = sum / static_cast<float>(std::max(1, to - from));
                frame.bands[static_cast<size_t>(band)] =
                    dbToUnit(magnitudeToDb(mean / static_cast<float>(fftSize)));
            }
            ++completed;
            if (progress && (f % 64) == 0) {
                progress(static_cast<float>(completed.load()) /
                         static_cast<float>(std::max<long long>(1, frameCount)));
            }
        }
    };

    for (int i = 0; i < chunks; ++i) workers.emplace_back(worker, i);
    for (auto &thread : workers) {
        if (thread.joinable()) thread.join();
    }
    if (progress) progress(1.0f);

    // ---- post pass: per-band auto level, level envelope, onset detection ----
    std::vector<float> bandMax(static_cast<size_t>(bandCount), 0.05f);
    for (const auto &frame : result->frames) {
        for (int b = 0; b < bandCount; ++b) {
            bandMax[static_cast<size_t>(b)] =
                std::max(bandMax[static_cast<size_t>(b)], frame.bands[static_cast<size_t>(b)]);
        }
    }
    float fluxMax = 1e-6f;
    float rmsMax = 1e-6f;
    for (const auto &frame : result->frames) {
        fluxMax = std::max(fluxMax, frame.flux);
        rmsMax = std::max(rmsMax, frame.rms);
    }
    for (size_t i = 0; i < result->frames.size(); ++i) {
        AnalysisFrame &frame = result->frames[i];
        for (int b = 0; b < bandCount; ++b) {
            const float scale = std::max(bandMax[static_cast<size_t>(b)], 0.08f);
            frame.bands[static_cast<size_t>(b)] =
                std::clamp(frame.bands[static_cast<size_t>(b)] / scale, 0.0f, 1.0f);
        }
        frame.flux = std::clamp(frame.flux / fluxMax, 0.0f, 1.0f);
        frame.rms = std::clamp(frame.rms / rmsMax, 0.0f, 1.0f);
        // level: smoothed rms
        float levelTarget = std::pow(frame.rms, 0.6f);
        frame.level = levelTarget;
        // onset: flux above a local moving average
        const size_t window = 12;
        float localSum = 0.0f;
        int count = 0;
        for (size_t k = (i > window ? i - window : 0); k < std::min(result->frames.size(), i + window); ++k) {
            localSum += result->frames[k].flux;
            ++count;
        }
        const float local = count > 0 ? localSum / static_cast<float>(count) : 0.0f;
        const float excess = frame.flux - local;
        frame.onset = std::clamp(excess * 3.2f, 0.0f, 1.0f);
    }
    // Smooth the level a little so it does not jitter.
    float smoothedLevel = 0.0f;
    for (auto &frame : result->frames) {
        smoothedLevel = smoothedLevel + (frame.level - smoothedLevel) * 0.35f;
        frame.level = smoothedLevel;
    }

    result->computeSeconds = GetTime() - startTime;
    return result;
}

AnalysisPtr analyzeWindow(const AudioBuffer &buffer, double windowEnd,
                          const AnalysisSettings &settings, const AudioPtr &source) {
    auto result = std::make_shared<AnalysisData>();
    if (buffer.frameCount <= 0 || buffer.sampleRate <= 0) return result;

    const int fftSize = std::max(256, settings.fftSize);
    const int bins = fftSize / 2;
    const int spectrumBins = std::max(16, settings.spectrumBins);
    const int bandCount = std::max(4, settings.bandCount);
    const int sampleRate = buffer.sampleRate;

    result->sampleRate = sampleRate;
    result->fftSize = fftSize;
    result->hopSize = fftSize;  // one frame: the window itself
    result->bandCount = bandCount;
    result->spectrumBins = spectrumBins;
    result->duration = buffer.duration();
    result->source = source;

    // Causal window: the newest FFT-sized slice available at `time`. Rendered
    // buffers only carry frames up to the current evaluation, so the window is
    // zero-padded instead of reading past the end.
    const long long endLocal = std::clamp<long long>(
        std::llround(windowEnd * sampleRate) - buffer.startFrame, 0, buffer.frameCount);
    const long long startLocal = std::max<long long>(0, endLocal - fftSize);
    result->originTime =
        static_cast<double>(startLocal + buffer.startFrame) / static_cast<double>(sampleRate);

    result->frames.resize(1);
    result->spectrum.assign(static_cast<size_t>(spectrumBins), 0.0f);
    AnalysisFrame &frame = result->frames[0];
    frame.bands.assign(static_cast<size_t>(bandCount), 0.0f);

    std::vector<float> window(static_cast<size_t>(fftSize), 0.0f);
    float sumSquares = 0.0f;
    for (int i = 0; i < fftSize; ++i) {
        const long long local = startLocal + i;
        if (local >= buffer.frameCount) break;
        window[static_cast<size_t>(i)] = buffer.monoAt(static_cast<double>(local));
        sumSquares += window[static_cast<size_t>(i)] * window[static_cast<size_t>(i)];
    }
    const float rms = std::sqrt(sumSquares / static_cast<float>(fftSize));
    frame.rms = dbToUnit(magnitudeToDb(rms));
    frame.level = std::pow(frame.rms, 0.6f);

    applyHannWindow(window.data(), fftSize);
    std::vector<float> magnitudes(static_cast<size_t>(bins), 0.0f);
    Fft &fft = sharedFft(fftSize);
    fft.magnitude(window.data(), magnitudes.data());

    const float nyquist = static_cast<float>(sampleRate) * 0.5f;
    const float low = std::max(10.0f, settings.lowFrequency);
    const float high = std::min(nyquist, std::max(low * 2.0f, settings.highFrequency));
    const std::vector<int> spectrumEdges =
        logBinEdges(spectrumBins, bins, low, high, nyquist);
    const std::vector<int> bandEdges = logBinEdges(bandCount, bins, low, high, nyquist);
    float *row = result->spectrum.data();
    for (int s = 0; s < spectrumBins; ++s) {
        const int from = spectrumEdges[static_cast<size_t>(s)];
        const int to = std::max(from + 1, spectrumEdges[static_cast<size_t>(s + 1)]);
        float sum = 0.0f;
        for (int b = from; b < to && b < bins; ++b) sum += magnitudes[static_cast<size_t>(b)];
        const float mean = sum / static_cast<float>(std::max(1, to - from));
        row[s] = dbToUnit(magnitudeToDb(mean / static_cast<float>(fftSize)));
    }
    for (int band = 0; band < bandCount; ++band) {
        const int from = bandEdges[static_cast<size_t>(band)];
        const int to = std::max(from + 1, bandEdges[static_cast<size_t>(band + 1)]);
        float sum = 0.0f;
        for (int b = from; b < to && b < bins; ++b) sum += magnitudes[static_cast<size_t>(b)];
        const float mean = sum / static_cast<float>(std::max(1, to - from));
        frame.bands[static_cast<size_t>(band)] =
            dbToUnit(magnitudeToDb(mean / static_cast<float>(fftSize)));
    }
    return result;
}

}  // namespace pf
