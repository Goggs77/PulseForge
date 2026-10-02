// Timeline: audio overview, time ruler and playhead scrubbing.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "render/Palette.h"
#include "ui/App.h"

namespace pf {

namespace {

float kRulerHeight() { return ui::s(22.0f); }
float kWaveHeight() { return ui::s(70.0f); }

std::string formatTime(double seconds) {
    const int minutes = static_cast<int>(seconds) / 60;
    const double rest = seconds - minutes * 60;
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%d:%05.2f", minutes, rest);
    return buffer;
}

}  // namespace

void drawTimeline(UiState &state, Rectangle bounds) {
    const ui::Theme &t = ui::theme();
    Project &project = state.project;
    const double duration = project.effectiveDuration(state.clip.duration());

    ui::panel(bounds);
    char header[160];
    std::snprintf(header, sizeof(header), "Timeline  -  %.2f s @ %.0f fps  -  %d frames",
                  duration, project.video.fps,
                  static_cast<int>(std::ceil(duration * project.video.fps)));
    ui::drawText(Rectangle{bounds.x + ui::s(10.0f), bounds.y, bounds.width - ui::s(20.0f), ui::s(26.0f)}, header, 14.0f,
                 t.text, ui::Align::Left);

    const Rectangle area{bounds.x + 70.0f, bounds.y + 28.0f, bounds.width - 82.0f,
                         bounds.height - 34.0f};
    if (area.width < ui::s(40.0f) || area.height < ui::s(40.0f)) return;
    const Rectangle ruler{area.x, area.y, area.width, kRulerHeight()};
    const Rectangle wave{area.x, area.y + kRulerHeight(), area.width,
                         std::min(kWaveHeight(), area.height - kRulerHeight() - ui::s(4.0f))};
    const Rectangle lanes{area.x, wave.y + wave.height + 4.0f, area.width,
                          std::max(0.0f, area.y + area.height - (wave.y + wave.height + 4.0f))};

    auto timeToX = [&](double seconds) {
        return area.x + static_cast<float>(seconds / std::max(1e-6, duration)) * area.width;
    };
    auto xToTime = [&](float x) {
        return std::clamp(static_cast<double>((x - area.x) / area.width) * duration, 0.0, duration);
    };

    // ---- ruler ------------------------------------------------------------
    DrawRectangleRounded(ruler, 0.3f, 4, palette::modulate(t.panelAlt, 0.9f));
    const double fps = std::max(1.0, project.video.fps);
    double step = 1.0;
    const double candidates[] = {0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0};
    for (double candidate : candidates) {
        if (static_cast<float>(candidate / duration) * area.width > 64.0f) {
            step = candidate;
            break;
        }
        step = candidate;
    }
    for (double time = 0.0; time <= duration + 1e-9; time += step) {
        const float x = timeToX(time);
        DrawLine(static_cast<int>(x), static_cast<int>(ruler.y + ruler.height * 0.45f),
                 static_cast<int>(x), static_cast<int>(ruler.y + ruler.height),
                 palette::withAlpha(t.textDim, 0.85f));
        ui::drawText(Rectangle{x, ruler.y, 60.0f, ruler.height * 0.55f},
                     formatTime(time).c_str(), 10.0f, t.textDim);
    }

    // ---- waveform ---------------------------------------------------------
    DrawRectangleRounded(wave, 0.06f, 4, palette::modulate(t.panelAlt, 0.8f));
    const int buckets = state.clip.overviewBuckets();
    if (buckets > 0) {
        const float centerY = wave.y + wave.height * 0.5f;
        const float half = wave.height * 0.46f;
        const int columns = std::max(1, static_cast<int>(wave.width));
        for (int i = 0; i < columns; ++i) {
            const int bucket = std::min(buckets - 1,
                                        static_cast<int>(static_cast<float>(i) / columns * buckets));
            const float lo = state.clip.overviewMin(bucket);
            const float hi = state.clip.overviewMax(bucket);
            const float x = wave.x + static_cast<float>(i);
            if (hi - lo < 0.002f) {
                DrawLine(static_cast<int>(x), static_cast<int>(centerY - 1.0f),
                         static_cast<int>(x), static_cast<int>(centerY + 1.0f),
                         palette::withAlpha(t.accent, 0.35f));
                continue;
            }
            DrawLine(static_cast<int>(x), static_cast<int>(centerY - hi * half),
                     static_cast<int>(x), static_cast<int>(centerY - lo * half),
                     palette::withAlpha(palette::mix(t.accent, t.accentAlt, 0.35f), 0.75f));
        }
    } else {
        ui::drawText(wave, "No audio loaded - use Load audio or drop a file path in the inspector",
                     12.0f, palette::withAlpha(t.textDim, 0.85f), ui::Align::Center);
    }

    // Automation lives in its own block now: the curve is edited in the
    // inspector, so the free space below the waveform just advertises it.
    if (lanes.height > 20.0f) {
        for (const Node &node : project.graph.nodes) {
            if (node.kind != "mod.automation") continue;
            char hint[200];
            std::snprintf(hint, sizeof(hint),
                          "%s: select the block to edit its curve in the inspector",
                          node.displayTitle().c_str());
            ui::drawText(Rectangle{lanes.x, lanes.y, lanes.width, 18.0f}, hint, 11.0f,
                         palette::withAlpha(t.textDim, 0.9f));
            break;
        }
    }
    // ---- loop / trim markers ---------------------------------------------
    const double trimStart = project.video.useAudioDuration ? project.video.trimStart : 0.0;
    const float trimX = timeToX(static_cast<double>(trimStart));
    DrawLine(static_cast<int>(trimX), static_cast<int>(ruler.y), static_cast<int>(trimX),
             static_cast<int>(wave.y + wave.height), palette::withAlpha(t.warn, 0.9f));

    // ---- playhead and scrubbing ------------------------------------------
    const float playX = timeToX(state.playhead);
    DrawLine(static_cast<int>(playX), static_cast<int>(ruler.y), static_cast<int>(playX),
             static_cast<int>(area.y + area.height), t.danger);
    DrawTriangle(Vector2{playX - 6.0f, ruler.y - 2.0f}, Vector2{playX + 6.0f, ruler.y - 2.0f},
                 Vector2{playX, ruler.y + 8.0f}, t.danger);

    static bool sScrub = false;
    // A popup owns the mouse while it is open, so the timeline stays inert.
    if (!ui::inputBlocked() && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
        (ui::hovered(ruler) || ui::hovered(wave))) {
        sScrub = true;
    }
    if (ui::inputBlocked() || !IsMouseButtonDown(MOUSE_BUTTON_LEFT)) sScrub = false;
    if (sScrub) {
        state.playhead = xToTime(GetMouseX());
        state.clip.seek(state.playhead + trimStart);
    }
}

}  // namespace pf
