// Offline spectral analysis: FFT frames turned into bands and control signals.
#pragma once

#include <functional>
#include <vector>

#include "core/Port.h"

namespace pf {

struct AnalysisSettings {
    int fftSize = 2048;
    int hopSize = 512;
    int bandCount = 64;
    int spectrumBins = 256;
    float lowFrequency = 20.0f;
    float highFrequency = 18000.0f;
};

// `progress` receives 0..1, return false from it to abort.
AnalysisPtr analyzeAudio(const AudioBuffer &buffer, const AnalysisSettings &settings,
                         const std::function<void(float)> &progress = {},
                         const AudioPtr &source = {});

// Analyses one FFT window ending at `windowEnd` (seconds in clip time). Used by
// the Spectrum Analyzer for a processed Audio input, where no whole-file
// analysis exists. The returned data holds a single frame and records where its
// window starts so time lookups land on that frame.
AnalysisPtr analyzeWindow(const AudioBuffer &buffer, double windowEnd,
                          const AnalysisSettings &settings, const AudioPtr &source = {});

}  // namespace pf
