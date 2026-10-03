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
constexpr float kVisualHeightVumeter = 74.0f;
constexpr float kVisualHeightGraph = 52.0f;
constexpr float kVisualHeightGuard = 46.0f;
constexpr float kVisualHeightRing = 46.0f;
constexpr float kVisualHeightFilter = 84.0f;
// 4:3 dBFS graph plus a readout row; the plot itself is fitted to 4:3 while
// drawing, this is the body height that gives it room at zoom 1.
constexpr float kVisualHeightDynamics = 140.0f;

// Number of columns the spectrum is reduced to. 64 columns with bilinear-ish
// interpolation is the sweet spot for legibility versus per-frame cost.
constexpr int kSpectrumColumns = 64;
constexpr int kWaveColumns = 128;
constexpr int kCurvePoints = 72;

Color withAlpha(Color color, float alpha) { return palette::withAlpha(color, alpha); }

// Element sizes inside a block's live content follow the block itself, with
// clamps so text stays legible and handles stay grabbable at any canvas zoom.
// Deriving them from the body (which is already scaled by the canvas zoom) keeps
// every block internally consistent instead of mixing zoomed and fixed sizes.
float visualInset(const Rectangle &body) {
    return std::clamp(std::min(body.width, body.height) * 0.035f, 1.5f, 8.0f);
}

float visualFont(const Rectangle &body, float ratio) {
    return std::clamp(body.height * ratio, 7.0f, 19.0f);
}

float visualHandle(const Rectangle &body, float ratio, float lo, float hi) {
    return std::clamp(std::min(body.width, body.height) * ratio, lo, hi);
}

float visualStroke(const Rectangle &body, float ratio) {
    return std::clamp(std::min(body.width, body.height) * ratio, 1.0f, 3.0f);
}

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
    // Follow the analysis this block actually produced from its Audio input,
    // not the project's imported one.
    const AnalysisPtr &analysis =
        node.outputs.empty() ? AnalysisPtr() : node.outputs[0].analysis;
    const float *row = analysis ? analysis->spectrumRow(state.frameContext.audioTime) : nullptr;
    const int bins = analysis ? analysis->spectrumBins : 0;
    if (!row || bins <= 0) {
        ui::drawText(inner, "no analysis", visualFont(inner, 0.18f), withAlpha(t.textDim, 0.8f),
                     ui::Align::Center);
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
        ui::drawText(inner, "waiting for audio", visualFont(inner, 0.18f), withAlpha(t.textDim, 0.8f),
                     ui::Align::Center);
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
        ui::drawText(inner, "no audio loaded", visualFont(inner, 0.20f),
                     withAlpha(t.textDim, 0.8f), ui::Align::Center);
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

// ---------------------------------------------------------------------------
// Debug: VU / digital meter and guard lamps
// ---------------------------------------------------------------------------

// VU scale: -20 .. +18 VU, where 0 VU = -18 dBFS.
constexpr float kVuMin = -20.0f;
constexpr float kVuMax = 18.0f;

void drawMeterVisual(UiState &state, const Node &node, Rectangle body, float zoom) {
    (void)zoom;
    const ui::Theme &t = ui::theme();
    const float inset = visualInset(body);
    const Rectangle inner{body.x + inset, body.y + inset, body.width - inset * 2.0f,
                          body.height - inset * 2.0f};
    drawVisualBackground(body);

    auto reading = [&](const char *key, double fallback) {
        const auto it = node.runtimeState.find(key);
        return it == node.runtimeState.end() ? fallback : it->second;
    };
    const double dbfs = reading("db", -120.0);
    const double vu = reading("vu", -120.0);
    const double needle = std::clamp(reading("needle", vu), static_cast<double>(kVuMin),
                                     static_cast<double>(kVuMax));
    const double peak = std::clamp(reading("peak", vu), static_cast<double>(kVuMin),
                                   static_cast<double>(kVuMax));

    if (node.pint("mode", 0) == 1) {
        // Value/time diagram: the same shape the Frequency Band block uses.
        const int capacity = static_cast<int>(node.historyA.size());
        if (capacity < 2) {
            ui::drawText(inner, "waiting for a signal", visualFont(inner, 0.16f),
                         withAlpha(t.textDim, 0.8f), ui::Align::Center);
            return;
        }
        static thread_local std::vector<float> series;
        series.assign(static_cast<size_t>(capacity), 0.0f);
        for (int i = 0; i < capacity; ++i) {
            const float position = static_cast<float>(i) / static_cast<float>(capacity - 1);
            series[static_cast<size_t>(i)] =
                std::clamp(0.5f + 0.5f * Node::historyAt(node.historyA, capacity,
                                                         node.historyCount, position),
                           0.0f, 1.0f);
        }
        const float midY = inner.y + inner.height * 0.5f;
        DrawLine(static_cast<int>(inner.x), static_cast<int>(midY),
                 static_cast<int>(inner.x + inner.width), static_cast<int>(midY),
                 withAlpha(t.border, 0.6f));
        drawFilledSeries(inner, series.data(), capacity, withAlpha(t.accent, 0.7f),
                         withAlpha(t.accent, 0.08f));
        drawSeriesOutline(inner, series.data(), capacity, withAlpha(t.accent, 0.95f), 0.0f, 1.0f);
        char text[64];
        std::snprintf(text, sizeof(text), "%.3f   %.1f dBFS",
                      Node::historyAt(node.historyA, capacity, node.historyCount, 1.0f),
                      static_cast<float>(dbfs));
        ui::drawTextClipped(Rectangle{inner.x + inset, inner.y, inner.width - inset * 2.0f,
                                      visualFont(inner, 0.16f) + 2.0f},
                            text, visualFont(inner, 0.13f), withAlpha(t.text, 0.9f));
        return;
    }

    // Classic VU movement: an arc of radius 1.5x the strip height drawn from a
    // pivot below the strip, so only the top of the dial is visible.
    const float pivotX = inner.x + inner.width * 0.5f;
    const float pivotY = inner.y + inner.height * 1.85f;
    const float radius = inner.height * 1.6f;
    const float startAngle = 212.0f;
    const float endAngle = 328.0f;
    auto angleFor = [&](float value) {
        const float fraction = std::clamp((value - kVuMin) / (kVuMax - kVuMin), 0.0f, 1.0f);
        return (startAngle + (endAngle - startAngle) * fraction) * DEG2RAD;
    };
    auto pointOn = [&](float value, float scale) {
        const float angle = angleFor(value);
        return Vector2{pivotX + std::cos(angle) * radius * scale,
                       pivotY + std::sin(angle) * radius * scale};
    };

    // Arc, ticks and the accented 0 VU mark.
    const int segments = 48;
    Vector2 previous = pointOn(kVuMin, 1.0f);
    for (int i = 1; i <= segments; ++i) {
        const float value = kVuMin + (kVuMax - kVuMin) * static_cast<float>(i) / segments;
        const Vector2 point = pointOn(value, 1.0f);
        DrawLineEx(previous, point, visualStroke(inner, 0.02f), withAlpha(t.border, 0.9f));
        previous = point;
    }
    const float tickValues[] = {-20.0f, -10.0f, -5.0f, -3.0f, -1.0f, 0.0f, 1.0f, 3.0f, 6.0f, 10.0f, 18.0f};
    for (const float tick : tickValues) {
        const bool zero = std::fabs(tick) < 0.01f;
        const Vector2 outer = pointOn(tick, 1.0f);
        const Vector2 innerPoint = pointOn(tick, 0.88f);
        DrawLineEx(outer, innerPoint, zero ? visualStroke(inner, 0.03f) : visualStroke(inner, 0.015f),
                   zero ? t.accent : withAlpha(t.textDim, 0.9f));
    }
    // Peak hold marker.
    const Vector2 peakPoint = pointOn(static_cast<float>(peak), 1.04f);
    DrawCircleV(peakPoint, visualHandle(inner, 0.028f, 1.2f, 3.0f), withAlpha(t.warn, 0.95f));

    // Needle: green below 0 VU, amber up to +6, red above.
    const Color needleInk = needle > 6.0 ? t.danger : (needle > 0.0 ? t.warn : t.success);
    const Vector2 needleStart = pointOn(static_cast<float>(needle), 0.28f);
    const Vector2 needleEnd = pointOn(static_cast<float>(needle), 0.94f);
    DrawLineEx(needleStart, needleEnd, visualStroke(inner, 0.03f), needleInk);

    char text[96];
    // Kept short: "0 VU = -18 dBFS" lives in the block description and README.
    std::snprintf(text, sizeof(text), "%+.1f dB   %+.1f VU", static_cast<float>(dbfs),
                  static_cast<float>(vu));
    const float readoutHeight = visualFont(inner, 0.16f) + 2.0f;
    ui::drawTextClipped(Rectangle{inner.x + inset, inner.y + inner.height - readoutHeight,
                                  inner.width - inset * 2.0f, readoutHeight},
                        text, visualFont(inner, 0.13f),
                        dbfs > 0.0f ? t.danger : withAlpha(t.text, 0.92f));
}

void drawGuardVisual(UiState &state, const Node &node, Rectangle body, float zoom) {
    (void)state;
    (void)zoom;
    const ui::Theme &t = ui::theme();
    const float inset = visualInset(body);
    const Rectangle inner{body.x + inset, body.y + inset, body.width - inset * 2.0f,
                          body.height - inset * 2.0f};
    drawVisualBackground(body);

    struct Lamp {
        const char *key;
        const char *label;
        Color colour;
    };
    const Lamp lamps[] = {
        {"pos", "+\u221E", t.danger},
        {"neg", "-\u221E", t.warn},
        {"nan", "NaN", t.accentAlt},
    };
    const int count = 3;
    const float slot = inner.width / static_cast<float>(count);
    const float lampY = inner.y + inner.height * 0.36f;
    const float radius = visualHandle(inner, 0.26f, 3.0f, 14.0f);
    const float labelGap = visualHandle(inner, 0.06f, 1.5f, 5.0f);
    const float labelFont = visualFont(inner, 0.26f);
    for (int i = 0; i < count; ++i) {
        const auto it = node.runtimeState.find(lamps[i].key);
        const float level = it == node.runtimeState.end() ? 0.0f : static_cast<float>(it->second);
        const float cx = inner.x + slot * (static_cast<float>(i) + 0.5f);
        const Color dark = palette::modulate(lamps[i].colour, 0.22f);
        DrawCircleV(Vector2{cx, lampY}, radius, palette::mix(dark, lamps[i].colour, level));
        DrawCircleLinesV(Vector2{cx, lampY}, radius,
                         withAlpha(level > 0.05f ? lamps[i].colour : t.border, 0.95f));
        if (level > 0.35f) {
            DrawCircleV(Vector2{cx, lampY}, radius * 0.45f, withAlpha(WHITE, level * 0.85f));
        }
        ui::drawText(Rectangle{cx - slot * 0.5f, lampY + radius + labelGap, slot,
                               visualFont(inner, 0.32f)},
                     lamps[i].label, labelFont,
                     withAlpha(level > 0.05f ? lamps[i].colour : t.textDim, 0.95f),
                     ui::Align::Center, level > 0.05f);
    }
}

// ---------------------------------------------------------------------------
// Modulation: ring buffer contents and the signal filter response
// ---------------------------------------------------------------------------

void drawRingbufferVisual(UiState &state, const Node &node, Rectangle body, float zoom) {
    (void)state;
    (void)zoom;
    const ui::Theme &t = ui::theme();
    const float inset = visualInset(body);
    const Rectangle inner{body.x + inset, body.y + inset, body.width - inset * 2.0f,
                          body.height - inset * 2.0f};
    drawVisualBackground(body);

    const int capacity = static_cast<int>(node.historyA.size());
    if (capacity < 2) {
        ui::drawText(inner, "buffer empty", visualFont(inner, 0.25f), withAlpha(t.textDim, 0.8f),
                     ui::Align::Center);
        return;
    }
    static thread_local std::vector<float> series;
    series.assign(static_cast<size_t>(capacity), 0.0f);
    float sum = 0.0f;
    for (int i = 0; i < capacity; ++i) {
        const float position = static_cast<float>(i) / static_cast<float>(capacity - 1);
        const float value = Node::historyAt(node.historyA, capacity, node.historyCount, position);
        series[static_cast<size_t>(i)] = std::clamp(0.5f + 0.5f * value, 0.0f, 1.0f);
        sum += value;
    }
    const float average = sum / static_cast<float>(capacity);
    const float midY = inner.y + inner.height * 0.5f;
    DrawLine(static_cast<int>(inner.x), static_cast<int>(midY),
             static_cast<int>(inner.x + inner.width), static_cast<int>(midY),
             withAlpha(t.border, 0.6f));
    drawFilledSeries(inner, series.data(), capacity, withAlpha(t.accent, 0.55f),
                     withAlpha(t.accent, 0.06f));
    drawSeriesOutline(inner, series.data(), capacity, withAlpha(t.accent, 0.9f), 0.0f, 1.0f);

    const float averageY = midY - std::clamp(average, -1.0f, 1.0f) * inner.height * 0.5f;
    DrawLineEx(Vector2{inner.x, averageY}, Vector2{inner.x + inner.width, averageY},
               visualStroke(inner, 0.02f), withAlpha(t.accentAlt, 0.9f));

    const auto phaseIt = node.runtimeState.find("phase");
    const float phase = phaseIt == node.runtimeState.end() ? 0.0f : static_cast<float>(phaseIt->second);
    const float readX = inner.x + std::clamp(phase, 0.0f, 1.0f) * inner.width;
    DrawLineEx(Vector2{readX, inner.y}, Vector2{readX, inner.y + inner.height},
               visualStroke(inner, 0.02f), withAlpha(t.warn, 0.85f));
}

// Filter response plot. The y axis is the pivot's resonance mapping: Q = 10^(dB/20).
constexpr float kFilterDbMax = 18.0f;
constexpr float kFilterDbMin = -36.0f;

void filterPlotRects(Rectangle body, Rectangle *inner) {
    const float inset = visualInset(body);
    *inner = Rectangle{body.x + inset, body.y + inset, body.width - inset * 2.0f,
                       body.height - inset * 2.0f};
}

void drawFilterVisual(UiState &state, const Node &node, Rectangle body, float zoom) {
    (void)zoom;
    const ui::Theme &t = ui::theme();
    Rectangle inner;
    filterPlotRects(body, &inner);
    drawVisualBackground(body);

    const double fs = std::max(1.0f, state.frameContext.fps);
    const double nyquist = std::max(1.0, fs * 0.5);
    const double fMin = 0.01;
    const double fMax = std::max(fMin * 2.0, nyquist);
    const int mode = std::clamp(node.pint("mode", 0), 0, 2);
    // Follow the modulated values the block actually used, so the plot (and its
    // pivot) track an LFO patched into Cutoff or Resonance.
    float effective = 0.0f;
    const float baseQ = node.effectiveParam("resonance", &effective) ? effective
                                                                    : node.pfloat("resonance", 0.707f);
    const double q = std::clamp(static_cast<double>(baseQ), 0.05, 20.0);
    const float baseCutoff =
        node.effectiveParam("cutoff", &effective) ? effective : node.pfloat("cutoff", 4.0f);
    const double cutoff = std::clamp(static_cast<double>(baseCutoff), fMin, fMax);
    const BiquadCoefficients coefficients = biquadCoefficients(mode, cutoff, q, fs);

    auto xFor = [&](double hz) {
        const double position = std::log(std::max(hz, fMin) / fMin) / std::log(fMax / fMin);
        return inner.x + static_cast<float>(std::clamp(position, 0.0, 1.0)) * inner.width;
    };
    auto yFor = [&](double db) {
        const double position = (kFilterDbMax - db) / (kFilterDbMax - kFilterDbMin);
        return inner.y + static_cast<float>(std::clamp(position, 0.0, 1.0)) * inner.height;
    };

    // Grid: decades plus the 0 dB line.
    for (double decade = 0.01; decade <= fMax; decade *= 10.0) {
        const float x = xFor(decade);
        DrawLine(static_cast<int>(x), static_cast<int>(inner.y), static_cast<int>(x),
                 static_cast<int>(inner.y + inner.height), withAlpha(t.border, 0.45f));
    }
    for (double db : {-24.0, -12.0, 0.0}) {
        const float y = yFor(db);
        DrawLine(static_cast<int>(inner.x), static_cast<int>(y),
                 static_cast<int>(inner.x + inner.width), static_cast<int>(y),
                 withAlpha(db == 0.0 ? t.accent : t.border, db == 0.0 ? 0.55f : 0.4f));
    }

    // Magnitude response.
    const int points = 72;
    Vector2 previous{};
    for (int i = 0; i < points; ++i) {
        const double position = static_cast<double>(i) / static_cast<double>(points - 1);
        const double hz = fMin * std::pow(fMax / fMin, position);
        const double db = biquadMagnitudeDb(coefficients, hz, fs);
        const Vector2 point{xFor(hz), yFor(db)};
        if (i > 0) {
            DrawLineEx(previous, point, visualStroke(inner, 0.03f), withAlpha(t.accent, 0.95f));
        }
        previous = point;
    }

    // Cutoff marker and the interactive pivot (x = cutoff, y = resonance).
    const float cutoffX = xFor(cutoff);
    DrawLineEx(Vector2{cutoffX, inner.y}, Vector2{cutoffX, inner.y + inner.height},
               visualStroke(inner, 0.02f), withAlpha(t.warn, 0.45f));
    const float pivotX = cutoffX;
    const float pivotY = yFor(20.0 * std::log10(std::max(0.05, q)));
    const float pivotRadius = visualHandle(inner, 0.075f, 3.0f, 10.0f);
    DrawCircleV(Vector2{pivotX, pivotY}, pivotRadius * 1.55f, withAlpha(t.warn, 0.30f));
    DrawCircleV(Vector2{pivotX, pivotY}, pivotRadius, t.warn);
    DrawCircleLinesV(Vector2{pivotX, pivotY}, pivotRadius, withAlpha(BLACK, 0.6f));

    // Two short readouts instead of one long line: a bigger relative font can no
    // longer push the text out of the block.
    const float textHeight = visualFont(inner, 0.16f) + 2.0f;
    char text[64];
    std::snprintf(text, sizeof(text), "%.2f Hz   Q %.2f", cutoff, q);
    ui::drawTextClipped(Rectangle{inner.x + 2.0f, inner.y, inner.width * 0.62f, textHeight}, text,
                        visualFont(inner, 0.13f), withAlpha(t.text, 0.92f));
    ui::drawTextClipped(Rectangle{inner.x + inner.width * 0.62f, inner.y,
                                  inner.width * 0.38f - 2.0f, textHeight},
                        "drag pivot", visualFont(inner, 0.11f), withAlpha(t.textDim, 0.9f),
                        ui::Align::Right);
}

// Dynamics: a 4:3 value/time graph whose vertical axis runs from -60 dBFS
// (silence in practice) to 0 dBFS, with the dry input and the processed output
// loudness scrolling through it. The static trigger level is marked so the
// compression/expansion point is readable at a glance.
void drawDynamicsVisual(UiState &state, const Node &node, Rectangle body, float zoom) {
    (void)state;
    (void)zoom;
    const ui::Theme &t = ui::theme();
    const float inset = visualInset(body);
    const Rectangle inner{body.x + inset, body.y + inset, body.width - inset * 2.0f,
                          body.height - inset * 2.0f};
    drawVisualBackground(body);

    auto reading = [&](const char *key, double fallback) {
        const auto it = node.runtimeState.find(key);
        return it == node.runtimeState.end() ? fallback : it->second;
    };
    const float textHeight = visualFont(inner, 0.11f) + 2.0f;
    // Split readout: each label owns half the row so neither can push the other
    // out of the block, whatever the GUI scale is.
    char text[48];
    std::snprintf(text, sizeof(text), "DRY %.1f", reading("dryDb", -120.0));
    ui::drawTextClipped(Rectangle{inner.x + 2.0f, inner.y, inner.width * 0.5f - 3.0f, textHeight},
                        text, visualFont(inner, 0.11f), withAlpha(t.textDim, 0.95f));
    std::snprintf(text, sizeof(text), "WET %.1f", reading("wetDb", -120.0));
    ui::drawTextClipped(Rectangle{inner.x + inner.width * 0.5f, inner.y,
                                  inner.width * 0.5f - 2.0f, textHeight},
                        text, visualFont(inner, 0.11f), withAlpha(t.accent, 0.95f),
                        ui::Align::Right);

    // 4:3 plot, fitted to both the block width and the leftover height.
    const float plotMaxHeight = std::max(8.0f, inner.height - textHeight);
    float plotHeight = plotMaxHeight;
    float plotWidth = plotHeight * 4.0f / 3.0f;
    if (plotWidth > inner.width) {
        plotWidth = inner.width;
        plotHeight = plotWidth * 3.0f / 4.0f;
    }
    const Rectangle plot{inner.x + (inner.width - plotWidth) * 0.5f, inner.y + textHeight,
                         plotWidth, plotHeight};
    DrawRectangleRounded(plot, ui::roundness(plot, ui::s(3.0f)), 3,
                         palette::modulate(t.panelAlt, 0.55f));

    auto yForDb = [&](double db) {
        const double position = std::clamp((db + 60.0) / 60.0, 0.0, 1.0);
        return plot.y + plot.height * static_cast<float>(1.0 - position);
    };
    const float gridFont = visualFont(plot, 0.10f);
    // The bottom line is the noise floor / -inf: everything quieter than the
    // floor sits on it, so it is labelled accordingly.
    const int gridDb[] = {0, -12, -24, -36, -48, -60};
    for (int db : gridDb) {
        const float y = yForDb(static_cast<double>(db));
        DrawLineEx(Vector2{plot.x, y}, Vector2{plot.x + plot.width, y},
                   visualStroke(plot, 0.012f), withAlpha(t.border, 0.55f));
        char label[8];
        if (db <= -60) {
            std::snprintf(label, sizeof(label), "-inf");
        } else {
            std::snprintf(label, sizeof(label), "%d", db);
        }
        const float labelHeight = gridFont + 1.0f;
        const float labelY =
            std::clamp(y - labelHeight * 0.5f, plot.y + 1.0f,
                       plot.y + plot.height - labelHeight - 1.0f);
        ui::drawTextClipped(Rectangle{plot.x + 2.0f, labelY, plot.width * 0.30f, labelHeight},
                            label, gridFont, withAlpha(t.textDim, 0.75f));
    }

    float thresholdDb = node.pfloat("threshold", -18.0f);
    node.effectiveParam("threshold", &thresholdDb);
    const float thresholdY = yForDb(static_cast<double>(thresholdDb));
    DrawLineEx(Vector2{plot.x, thresholdY}, Vector2{plot.x + plot.width, thresholdY},
               visualStroke(plot, 0.02f), withAlpha(t.warn, 0.60f));

    // Mode and gain reduction sit inside the plot's top-right corner, where the
    // signal never reaches.
    const Color modeColor = node.pint("mode", 0) == 1 ? t.accentAlt : t.accent;
    std::snprintf(text, sizeof(text), "%s %+.1f dB",
                  node.pint("mode", 0) == 1 ? "EXP" : "COMP", reading("gainReductionDb", 0.0));
    ui::drawTextClipped(Rectangle{plot.x + plot.width * 0.40f, plot.y + 1.0f,
                                  plot.width * 0.60f - 3.0f, gridFont + 2.0f},
                        text, gridFont, withAlpha(modeColor, 0.95f), ui::Align::Right);

    const int capacity = static_cast<int>(node.historyA.size());
    if (capacity < 2) {
        ui::drawText(plot, "waiting for audio", visualFont(plot, 0.14f), withAlpha(t.textDim, 0.8f),
                     ui::Align::Center);
        return;
    }
    static thread_local std::vector<float> drySeries;
    static thread_local std::vector<float> wetSeries;
    drySeries.assign(static_cast<size_t>(capacity), 0.0f);
    wetSeries.assign(static_cast<size_t>(capacity), 0.0f);
    for (int i = 0; i < capacity; ++i) {
        const float position = static_cast<float>(i) / static_cast<float>(capacity - 1);
        const float dryDb = Node::historyAt(node.historyA, capacity, node.historyCount, position);
        const float wetDb = Node::historyAt(node.historyB, capacity, node.historyCount, position);
        drySeries[static_cast<size_t>(i)] = std::clamp((dryDb + 60.0f) / 60.0f, 0.0f, 1.0f);
        wetSeries[static_cast<size_t>(i)] = std::clamp((wetDb + 60.0f) / 60.0f, 0.0f, 1.0f);
    }
    drawSeriesOutline(plot, drySeries.data(), capacity, withAlpha(t.textDim, 0.85f), 0.0f,
                      std::max(1.0f, visualStroke(plot, 0.015f)));
    drawFilledSeries(plot, wetSeries.data(), capacity, withAlpha(t.accent, 0.45f),
                     withAlpha(t.accent, 0.05f));
    drawSeriesOutline(plot, wetSeries.data(), capacity, withAlpha(t.accent, 0.95f), 0.0f,
                      std::max(1.2f, visualStroke(plot, 0.03f)));
}

}  // namespace

float nodeVisualHeight(const Node &node) {
    if (node.kind == "dsp.analyze") return kVisualHeightSpectrum;
    if (node.kind == "dsp.band") return kVisualHeightBand;
    if (node.kind == "mod.lfo" || node.kind == "mod.automation") return kVisualHeightCurve;
    if (node.kind == "src.audio") return kVisualHeightWave;
    if (node.kind == "dbg.meter") {
        return node.pint("mode", 0) == 1 ? kVisualHeightGraph : kVisualHeightVumeter;
    }
    if (node.kind == "dbg.guard") return kVisualHeightGuard;
    if (node.kind == "mod.ringbuffer") return kVisualHeightRing;
    if (node.kind == "mod.filter") return kVisualHeightFilter;
    if (node.kind == "dsp.dynamics") return kVisualHeightDynamics;
    return 0.0f;
}

void drawNodeVisual(UiState &state, const Node &node, Rectangle body, Rectangle clip, float zoom) {
    if (body.width < 8.0f || body.height < 8.0f) return;
    // Live content never spills outside its block *or* the panel it lives in, so
    // a block hanging over the edge of the canvas is clipped at the edge instead
    // of drawing over the neighbouring panels.
    Rectangle limit = body;
    if (clip.width > 0.0f && clip.height > 0.0f) {
        const float x0 = std::max(body.x, clip.x);
        const float y0 = std::max(body.y, clip.y);
        const float x1 = std::min(body.x + body.width, clip.x + clip.width);
        const float y1 = std::min(body.y + body.height, clip.y + clip.height);
        limit = Rectangle{x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
    }
    if (limit.width < 2.0f || limit.height < 2.0f) return;
    BeginScissorMode(static_cast<int>(limit.x), static_cast<int>(limit.y),
                     static_cast<int>(limit.width), static_cast<int>(limit.height));
    if (node.kind == "dsp.analyze") {
        drawSpectrumVisual(state, node, body, zoom);
    } else if (node.kind == "dsp.band") {
        drawBandVisual(state, node, body, zoom);
    } else if (node.kind == "mod.lfo" || node.kind == "mod.automation") {
        drawCurveVisual(state, node, body, zoom);
    } else if (node.kind == "src.audio") {
        drawWaveVisual(state, node, body, zoom);
    } else if (node.kind == "dbg.meter") {
        drawMeterVisual(state, node, body, zoom);
    } else if (node.kind == "dbg.guard") {
        drawGuardVisual(state, node, body, zoom);
    } else if (node.kind == "mod.ringbuffer") {
        drawRingbufferVisual(state, node, body, zoom);
    } else if (node.kind == "mod.filter") {
        drawFilterVisual(state, node, body, zoom);
    } else if (node.kind == "dsp.dynamics") {
        drawDynamicsVisual(state, node, body, zoom);
    }
    EndScissorMode();
}

bool nodeVisualHasPivot(const Node &node) { return node.kind == "mod.filter"; }

void nodeVisualPivotDrag(UiState &state, Node &node, Rectangle body, Vector2 mouse) {
    if (node.kind != "mod.filter") return;
    Rectangle inner;
    filterPlotRects(body, &inner);
    const double fs = std::max(1.0f, state.frameContext.fps);
    const double fMin = 0.01;
    const double fMax = std::max(fMin * 2.0, fs * 0.5);
    const float x = std::clamp((mouse.x - inner.x) / std::max(1.0f, inner.width), 0.0f, 1.0f);
    const float y = std::clamp((mouse.y - inner.y) / std::max(1.0f, inner.height), 0.0f, 1.0f);
    const double cutoff = fMin * std::pow(fMax / fMin, x);
    const double db = kFilterDbMax + (kFilterDbMin - kFilterDbMax) * y;
    const float q = std::clamp(static_cast<float>(std::pow(10.0, db / 20.0)), 0.05f, 20.0f);
    node.setFloat("cutoff", static_cast<float>(cutoff));
    node.setFloat("resonance", q);
    state.project.dirty = true;
}

}  // namespace pf
