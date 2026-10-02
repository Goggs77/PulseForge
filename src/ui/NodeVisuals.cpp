#include "ui/NodeVisuals.h"

#include <algorithm>
#include <cmath>

#include "core/Node.h"
#include "dsp/AudioClip.h"
#include "render/Palette.h"
#include "rlgl.h"
#include "ui/App.h"

namespace pf {

namespace {

constexpr float kVisualHeightSpectrum = 58.0f;
constexpr float kVisualHeightBand = 34.0f;
constexpr float kVisualHeightCurve = 64.0f;
constexpr float kVisualHeightWave = 40.0f;

// Number of columns the spectrum is reduced to. 64 columns with bilinear-ish
// interpolation is the sweet spot for legibility versus per-frame cost.
constexpr int kSpectrumColumns = 64;
constexpr int kWaveColumns = 128;
constexpr int kCurvePoints = 72;

Color withAlpha(Color color, float alpha) { return palette::withAlpha(color, alpha); }

// Filled strip built from a column array; one draw call for the whole shape.
void drawFilledSeries(Rectangle area, const float *values, int count, Color top, Color bottom,
                      float floorLevel = 0.0f) {
    if (count < 2) return;
    const float baseline = area.y + area.height * (1.0f - floorLevel);
    rlBegin(RL_QUADS);
    for (int i = 0; i < count - 1; ++i) {
        const float x0 = area.x + area.width * static_cast<float>(i) / static_cast<float>(count - 1);
        const float x1 =
            area.x + area.width * static_cast<float>(i + 1) / static_cast<float>(count - 1);
        const float y0 = baseline - area.height * std::clamp(values[i], 0.0f, 1.0f);
        const float y1 = baseline - area.height * std::clamp(values[i + 1], 0.0f, 1.0f);
        rlColor4ub(top.r, top.g, top.b, top.a);
        rlVertex2f(x0, y0);
        rlVertex2f(x1, y1);
        rlColor4ub(bottom.r, bottom.g, bottom.b, bottom.a);
        rlVertex2f(x1, baseline);
        rlVertex2f(x0, baseline);
    }
    rlEnd();
}

void drawSeriesOutline(Rectangle area, const float *values, int count, Color color, float floorLevel,
                       float thickness) {
    if (count < 2) return;
    const float baseline = area.y + area.height * (1.0f - floorLevel);
    Vector2 previous{area.x, baseline - area.height * std::clamp(values[0], 0.0f, 1.0f)};
    for (int i = 1; i < count; ++i) {
        const float x = area.x + area.width * static_cast<float>(i) / static_cast<float>(count - 1);
        const float y = baseline - area.height * std::clamp(values[i], 0.0f, 1.0f);
        DrawLineEx(previous, Vector2{x, y}, thickness, color);
        previous = Vector2{x, y};
    }
}

// Shared background so every block preview reads as the same kind of element.
void drawVisualBackground(Rectangle body) {
    const ui::Theme &t = ui::theme();
    DrawRectangleRounded(body, ui::roundness(body, ui::s(5.0f)), 4,
                         palette::modulate(t.panelAlt, 0.72f));
    DrawRectangleRoundedLines(body, ui::roundness(body, ui::s(5.0f)), 4,
                              withAlpha(t.border, 0.85f));
}

// ---------------------------------------------------------------------------
// Spectrum Analyzer: dry spectrum behind, gained + gated spectrum in front.
// ---------------------------------------------------------------------------
void drawSpectrumVisual(UiState &state, const Node &node, Rectangle body, float zoom) {
    (void)zoom;
    const ui::Theme &t = ui::theme();
    const Rectangle inner{body.x + ui::s(3.0f), body.y + ui::s(3.0f), body.width - ui::s(6.0f),
                          body.height - ui::s(6.0f)};
    drawVisualBackground(body);

    const float gain = node.pfloat("gain", 1.0f);
    const float gate = node.pfloat("gate", 0.0f);
    const AnalysisPtr &analysis = state.analysis;
    const float *row = analysis ? analysis->spectrumRow(state.frameContext.audioTime) : nullptr;
    const int bins = analysis ? analysis->spectrumBins : 0;
    if (!row || bins <= 0) {
        ui::drawText(inner, "no analysis", 10.0f, withAlpha(t.textDim, 0.8f), ui::Align::Center);
        return;
    }

    static thread_local std::vector<float> dry;
    static thread_local std::vector<float> wet;
    dry.assign(kSpectrumColumns, 0.0f);
    wet.assign(kSpectrumColumns, 0.0f);
    for (int column = 0; column < kSpectrumColumns; ++column) {
        const int from = column * bins / kSpectrumColumns;
        const int to = std::max(from + 1, (column + 1) * bins / kSpectrumColumns);
        float peak = 0.0f;
        for (int bin = from; bin < to && bin < bins; ++bin) peak = std::max(peak, row[bin]);
        dry[static_cast<size_t>(column)] = peak;
        const float shaped = peak * gain;
        wet[static_cast<size_t>(column)] = shaped < gate ? 0.0f : std::clamp(shaped, 0.0f, 1.0f);
    }

    // Grid: a couple of horizontal guides behind the data.
    for (int i = 1; i < 3; ++i) {
        const float y = inner.y + inner.height * static_cast<float>(i) / 3.0f;
        DrawLine(static_cast<int>(inner.x), static_cast<int>(y), static_cast<int>(inner.x + inner.width),
                 static_cast<int>(y), withAlpha(t.border, 0.5f));
    }
    // Dry first, as a dim filled area, then the processed spectrum on top.
    const Color dryColor = palette::mix(t.textDim, t.accent, 0.15f);
    drawFilledSeries(inner, dry.data(), kSpectrumColumns, withAlpha(dryColor, 0.55f),
                     withAlpha(dryColor, 0.10f));
    drawSeriesOutline(inner, dry.data(), kSpectrumColumns, withAlpha(dryColor, 0.85f), 0.0f, 1.0f);
    drawFilledSeries(inner, wet.data(), kSpectrumColumns, withAlpha(t.accent, 0.85f),
                     withAlpha(t.accent, 0.10f));
    drawSeriesOutline(inner, wet.data(), kSpectrumColumns, withAlpha(t.accent, 0.95f), 0.0f, 1.0f);

    // Tiny legend so the two series are identifiable.
    const float legendY = inner.y + 1.0f;
    DrawRectangle(static_cast<int>(inner.x + 3.0f), static_cast<int>(legendY), 6, 3,
                  withAlpha(dryColor, 0.9f));
    DrawRectangle(static_cast<int>(inner.x + 14.0f), static_cast<int>(legendY), 6, 3,
                  withAlpha(t.accent, 0.95f));
}

// ---------------------------------------------------------------------------
// Frequency Band: scrolling input/output bar with current-value markers.
// ---------------------------------------------------------------------------
void drawBandVisual(UiState &state, const Node &node, Rectangle body, float zoom) {
    (void)state;
    (void)zoom;
    const ui::Theme &t = ui::theme();
    const Rectangle inner{body.x + ui::s(3.0f), body.y + ui::s(3.0f), body.width - ui::s(6.0f),
                          body.height - ui::s(6.0f)};
    drawVisualBackground(body);

    const int capacity = node.historyA.empty() ? 0 : static_cast<int>(node.historyA.size());
    if (capacity <= 0) {
        ui::drawText(inner, "waiting for audio", 10.0f, withAlpha(t.textDim, 0.8f), ui::Align::Center);
        return;
    }
    static thread_local std::vector<float> input;
    static thread_local std::vector<float> output;
    input.assign(capacity, 0.0f);
    output.assign(capacity, 0.0f);
    for (int i = 0; i < capacity; ++i) {
        const float position = capacity > 1 ? static_cast<float>(i) / static_cast<float>(capacity - 1)
                                            : 1.0f;
        input[static_cast<size_t>(i)] = Node::historyAt(node.historyA, capacity, node.historyCount, position);
        output[static_cast<size_t>(i)] = Node::historyAt(node.historyB, capacity, node.historyCount, position);
    }
    const float baseline = inner.y + inner.height - 1.0f;
    DrawLine(static_cast<int>(inner.x), static_cast<int>(baseline), static_cast<int>(inner.x + inner.width),
             static_cast<int>(baseline), withAlpha(t.border, 0.8f));
    const Color inputColor = palette::mix(t.textDim, t.accent, 0.15f);
    drawFilledSeries(inner, input.data(), capacity, withAlpha(inputColor, 0.42f),
                     withAlpha(inputColor, 0.06f));
    drawSeriesOutline(inner, input.data(), capacity, withAlpha(inputColor, 0.75f), 0.0f, 1.0f);
    drawFilledSeries(inner, output.data(), capacity, withAlpha(t.accent, 0.85f),
                     withAlpha(t.accent, 0.12f));
    drawSeriesOutline(inner, output.data(), capacity, withAlpha(t.accent, 0.95f), 0.0f, 1.0f);

    // Current values as short bars pinned to the right edge.
    const float inValue = static_cast<float>(node.runtimeState.count("in") ? node.runtimeState.at("in") : 0.0);
    const float outValue =
        static_cast<float>(node.runtimeState.count("out") ? node.runtimeState.at("out") : 0.0);
    const float markerWidth = ui::s(3.0f);
    DrawRectangle(static_cast<int>(inner.x + inner.width - markerWidth),
                  static_cast<int>(inner.y + inner.height - std::clamp(inValue, 0.0f, 1.0f) * inner.height),
                  static_cast<int>(markerWidth),
                  static_cast<int>(std::clamp(inValue, 0.0f, 1.0f) * inner.height),
                  withAlpha(inputColor, 0.9f));
    DrawRectangle(static_cast<int>(inner.x + inner.width - markerWidth * 2.0f - 2.0f),
                  static_cast<int>(inner.y + inner.height - std::clamp(outValue, 0.0f, 1.0f) * inner.height),
                  static_cast<int>(markerWidth),
                  static_cast<int>(std::clamp(outValue, 0.0f, 1.0f) * inner.height),
                  withAlpha(t.accent, 0.95f));
}

// ---------------------------------------------------------------------------
// LFO and Automation: fixed curve with a pivot travelling along it.
// ---------------------------------------------------------------------------
void drawCurveVisual(UiState &state, const Node &node, Rectangle body, float zoom) {
    (void)zoom;
    const ui::Theme &t = ui::theme();
    const Rectangle inner{body.x + ui::s(3.0f), body.y + ui::s(3.0f), body.width - ui::s(6.0f),
                          body.height - ui::s(6.0f)};
    drawVisualBackground(body);

    static thread_local std::vector<float> samples;
    samples.assign(kCurvePoints, 0.0f);
    const bool isLfo = node.kind == "mod.lfo";
    const bool isAutomation = node.kind == "mod.automation";
    float pivot = 0.0f;
    if (isLfo) {
        // The whole shape is one cycle; the pivot walks along it at the LFO rate.
        const int shape = node.pint("shape", 0);
        for (int i = 0; i < kCurvePoints; ++i) {
            const float phase = static_cast<float>(i) / static_cast<float>(kCurvePoints - 1);
            float wave = 0.0f;
            switch (shape) {
                case 1: wave = 4.0f * std::fabs(phase - 0.5f) - 1.0f; break;
                case 2: wave = phase * 2.0f - 1.0f; break;
                case 3: wave = 1.0f - phase * 2.0f; break;
                case 4: wave = phase < 0.5f ? 1.0f : -1.0f; break;
                default: wave = std::sin(phase * 6.2831853f); break;
            }
            // Map -amplitude..+amplitude onto the strip with 0.5 as the centre.
            const float amplitude = std::max(0.05f, node.pfloat("amplitude", 1.0f));
            samples[static_cast<size_t>(i)] =
                std::clamp(0.5f + wave * 0.5f * std::min(1.0f, amplitude), 0.0f, 1.0f);
        }
        pivot = static_cast<float>(node.runtimeState.count("phase") ? node.runtimeState.at("phase") : 0.0);
    } else if (isAutomation) {
        const Param *curve = node.find("curve");
        for (int i = 0; i < kCurvePoints; ++i) {
            const double position = static_cast<double>(i) / static_cast<double>(kCurvePoints - 1);
            samples[static_cast<size_t>(i)] =
                curve ? std::clamp(curve->evalCurve(position), 0.0f, 1.0f) : 0.0f;
        }
        pivot = static_cast<float>(node.runtimeState.count("pos") ? node.runtimeState.at("pos") : 0.0);
    }

    // Mid line, the fixed curve, then the travelling pivot.
    const float midY = inner.y + inner.height * 0.5f;
    DrawLine(static_cast<int>(inner.x), static_cast<int>(midY), static_cast<int>(inner.x + inner.width),
             static_cast<int>(midY), withAlpha(t.border, 0.6f));
    drawFilledSeries(inner, samples.data(), kCurvePoints, withAlpha(t.accentAlt, 0.65f),
                     withAlpha(t.accentAlt, 0.08f));
    drawSeriesOutline(inner, samples.data(), kCurvePoints, withAlpha(t.accentAlt, 0.95f), 0.0f, 1.0f);

    const float pivotX = inner.x + inner.width * std::clamp(pivot, 0.0f, 1.0f);
    const int index = std::clamp(static_cast<int>(std::lround(
                                     std::clamp(pivot, 0.0f, 1.0f) * (kCurvePoints - 1))),
                                 0, kCurvePoints - 1);
    const float pivotY = inner.y + inner.height * (1.0f - samples[static_cast<size_t>(index)]);
    DrawCircleV(Vector2{pivotX, midY}, ui::s(4.5f), withAlpha(t.warn, 0.35f));
    DrawCircleV(Vector2{pivotX, pivotY}, ui::s(3.5f), t.warn);
    DrawCircleLinesV(Vector2{pivotX, pivotY}, ui::s(3.5f), withAlpha(BLACK, 0.6f));
    DrawLine(static_cast<int>(pivotX), static_cast<int>(inner.y), static_cast<int>(pivotX),
             static_cast<int>(inner.y + inner.height), withAlpha(t.warn, 0.35f));
}

// ---------------------------------------------------------------------------
// Audio Source: 0.1 second waveform window around the playhead.
// ---------------------------------------------------------------------------
void drawWaveVisual(UiState &state, const Node &node, Rectangle body, float zoom) {
    (void)node;
    (void)zoom;
    const ui::Theme &t = ui::theme();
    const Rectangle inner{body.x + ui::s(3.0f), body.y + ui::s(3.0f), body.width - ui::s(6.0f),
                          body.height - ui::s(6.0f)};
    drawVisualBackground(body);

    const AudioPtr &audio = state.clip.buffer();
    if (!audio || audio->frameCount <= 0) {
        ui::drawText(inner, "no audio loaded", 10.0f, withAlpha(t.textDim, 0.8f), ui::Align::Center);
        return;
    }
    const double window = 0.1;  // seconds shown left to right
    const double duration = audio->duration();
    // Keep the window inside the file so the strip is always filled, then let
    // the playhead marker slide when it runs into either edge.
    const double start = std::clamp(state.frameContext.audioTime - window * 0.5, 0.0,
                                    std::max(0.0, duration - window));
    const double secondsPerColumn = window / static_cast<double>(kWaveColumns);
    const double rate = audio->sampleRate;
    const float midY = inner.y + inner.height * 0.5f;
    const float half = inner.height * 0.46f;
    DrawLine(static_cast<int>(inner.x), static_cast<int>(midY),
             static_cast<int>(inner.x + inner.width), static_cast<int>(midY),
             withAlpha(t.border, 0.7f));
    const Color waveColor = withAlpha(t.accent, 0.9f);
    for (int column = 0; column < kWaveColumns; ++column) {
        // Work in seconds and convert once: mixing frame counts into the
        // column offset collapsed the whole window into the left edge.
        const double fromSeconds = start + secondsPerColumn * column;
        long long from = static_cast<long long>(fromSeconds * rate);
        long long to = static_cast<long long>((fromSeconds + secondsPerColumn) * rate);
        from = std::clamp(from, 0LL, audio->frameCount - 1);
        to = std::clamp(std::max(to, from + 1), from + 1, audio->frameCount);
        float lo = 0.0f;
        float hi = 0.0f;
        const int samples = std::clamp(static_cast<int>(to - from), 2, 24);
        for (int i = 0; i < samples; ++i) {
            const double frame = static_cast<double>(from) +
                                 static_cast<double>(to - from) * i / samples;
            const float value = audio->monoAt(frame);
            lo = std::min(lo, value);
            hi = std::max(hi, value);
        }
        const float x = inner.x + inner.width * static_cast<float>(column) /
                                      static_cast<float>(kWaveColumns - 1);
        const float yTop = midY - hi * half;
        const float yBottom = midY - lo * half;
        DrawLineEx(Vector2{x, yTop}, Vector2{x, yBottom + 0.5f}, 1.0f, waveColor);
    }
    // Playhead inside the window.
    const float playheadX =
        inner.x + inner.width * static_cast<float>(
                                  std::clamp((state.frameContext.audioTime - start) / window, 0.0, 1.0));
    DrawLine(static_cast<int>(playheadX), static_cast<int>(inner.y),
             static_cast<int>(playheadX), static_cast<int>(inner.y + inner.height),
             withAlpha(t.warn, 0.75f));
}

}  // namespace

float nodeVisualHeight(const Node &node) {
    if (node.kind == "dsp.analyze") return kVisualHeightSpectrum;
    if (node.kind == "dsp.band") return kVisualHeightBand;
    if (node.kind == "mod.lfo" || node.kind == "mod.automation") return kVisualHeightCurve;
    if (node.kind == "src.audio") return kVisualHeightWave;
    return 0.0f;
}

void drawNodeVisual(UiState &state, const Node &node, Rectangle body, float zoom) {
    if (body.width < 8.0f || body.height < 8.0f) return;
    if (node.kind == "dsp.analyze") {
        drawSpectrumVisual(state, node, body, zoom);
    } else if (node.kind == "dsp.band") {
        drawBandVisual(state, node, body, zoom);
    } else if (node.kind == "mod.lfo" || node.kind == "mod.automation") {
        drawCurveVisual(state, node, body, zoom);
    } else if (node.kind == "src.audio") {
        drawWaveVisual(state, node, body, zoom);
    }
}

}  // namespace pf
