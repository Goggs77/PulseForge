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
float kLaneHeight() { return ui::s(20.0f); }
float kHandleWidth() { return ui::s(9.0f); }
// Left margin: the time ruler starts here, and the slice lanes use it for their
// names.
float kGutter() { return ui::s(96.0f); }

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

    const Rectangle area{bounds.x + kGutter(), bounds.y + 28.0f,
                         bounds.width - kGutter() - ui::s(12.0f), bounds.height - 34.0f};
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

    // ---- slice automation lanes ------------------------------------------
    // Every Automation in Slice mode gets a lane under the waveform with two
    // draggable handles that are the block's Start and Finish times; the Slice
    // colour keeps several slices apart. The curve itself stays in the block and
    // inspector, the timeline only places the window.
    float laneY = lanes.y;
    const auto laneHandle = [&](const Rectangle &lane, float x, int end, const Node &node,
                                Color colour) {
        const Rectangle handle{x - kHandleWidth() * 0.5f, lane.y + ui::s(1.0f), kHandleWidth(),
                               lane.height - ui::s(2.0f)};
        const Rectangle grab{handle.x - ui::s(4.0f), lane.y, handle.width + ui::s(8.0f),
                             lane.height};
        const bool active = state.sliceDragNode == node.id && state.sliceDragEnd == end;
        const bool hot = active || (!ui::inputBlocked() && ui::hovered(grab));
        DrawRectangleRounded(handle, 0.6f, 3, palette::withAlpha(colour, hot ? 1.0f : 0.8f));
        if (!hot) return;
        char label[32];
        std::snprintf(label, sizeof(label), "%.2f s",
                      static_cast<double>(node.pfloat(end == 0 ? "start" : "finish", 0.0f)));
        const Rectangle labelRect{x - ui::s(30.0f), lane.y + ui::s(1.0f), ui::s(60.0f),
                                  lane.height - ui::s(2.0f)};
        DrawRectangleRounded(labelRect, 0.4f, 3, palette::withAlpha(BLACK, 0.6f));
        ui::drawText(labelRect, label, 10.0f, colour, ui::Align::Center);
    };
    for (Node &node : project.graph.nodes) {
        if (node.kind != "mod.automation" || !node.pbool("slice", false)) continue;
        if (laneY + kLaneHeight() > lanes.y + lanes.height) break;  // out of room
        const Rectangle lane{lanes.x, laneY, lanes.width, kLaneHeight()};
        const Color colour = node.pcolor("sliceColor");
        DrawRectangleRounded(lane, 0.35f, 3,
                             palette::withAlpha(palette::modulate(t.panelAlt, 0.72f), 0.8f));
        const double start =
            std::clamp(static_cast<double>(node.pfloat("start", 0.0f)), 0.0, duration);
        const double finish =
            std::clamp(static_cast<double>(node.pfloat("finish", 0.0f)), 0.0, duration);
        // Handles outside the visible timeline (a slice dragged past the end,
        // or a project with no media yet) park on the nearest edge so they stay
        // reachable.
        const float startX = std::clamp(timeToX(start), area.x, area.x + area.width);
        const float finishX = std::clamp(timeToX(finish), area.x, area.x + area.width);
        const float leftX = std::min(startX, finishX);
        const float rightX = std::max(startX, finishX);
        // The name lives in the gutter left of time 0, so a slice that starts at
        // the beginning never hides it.
        ui::drawTextClipped(Rectangle{bounds.x + ui::s(4.0f), lane.y,
                                      kGutter() - ui::s(10.0f), lane.height},
                            node.displayTitle().c_str(), 10.5f,
                            palette::withAlpha(colour, 0.95f), ui::Align::Right);
        DrawRectangleRounded(Rectangle{leftX, lane.y + ui::s(2.0f),
                                       std::max(ui::s(2.0f), rightX - leftX),
                                       lane.height - ui::s(4.0f)},
                             0.6f, 3, palette::withAlpha(colour, 0.3f));
        // Direction of travel: the curve plays from Start to Finish, so a slice
        // whose Start sits after its Finish reads as an arrow pointing back.
        if (std::fabs(finishX - startX) > ui::s(16.0f)) {
            const float arrowY = lane.y + lane.height * 0.5f;
            const float direction = finishX >= startX ? 1.0f : -1.0f;
            const float tipX = finishX - direction * ui::s(8.0f);
            DrawLineEx(Vector2{startX, arrowY}, Vector2{tipX, arrowY}, ui::s(1.0f),
                       palette::withAlpha(colour, 0.55f));
            DrawTriangle(Vector2{tipX + direction * ui::s(5.0f), arrowY},
                         Vector2{tipX - direction * ui::s(3.0f), arrowY - ui::s(3.5f)},
                         Vector2{tipX - direction * ui::s(3.0f), arrowY + ui::s(3.5f)},
                         palette::withAlpha(colour, 0.8f));
        }
        laneHandle(lane, startX, 0, node, colour);
        laneHandle(lane, finishX, 1, node, colour);
        if (!ui::inputBlocked() && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            const Rectangle grabs[2] = {
                Rectangle{startX - kHandleWidth() * 0.5f - ui::s(4.0f), lane.y,
                          kHandleWidth() + ui::s(8.0f), lane.height},
                Rectangle{finishX - kHandleWidth() * 0.5f - ui::s(4.0f), lane.y,
                          kHandleWidth() + ui::s(8.0f), lane.height}};
            for (int end = 0; end < 2; ++end) {
                if (!ui::hovered(grabs[end])) continue;
                state.sliceDragNode = node.id;
                state.sliceDragEnd = end;
                state.sliceDragGrab =
                    static_cast<float>(GetMouseX()) - (end == 0 ? startX : finishX);
                selectNode(state, node.id);
                break;
            }
        }
        laneY += kLaneHeight() + ui::s(4.0f);
    }
    if (lanes.height > 20.0f && laneY == lanes.y) {
        for (const Node &node : project.graph.nodes) {
            if (node.kind != "mod.automation") continue;
            char hint[220];
            std::snprintf(hint, sizeof(hint),
                          "%s: select the block to edit its curve, or turn on Slice mode to "
                          "place it on the timeline",
                          node.displayTitle().c_str());
            ui::drawText(Rectangle{lanes.x, lanes.y, lanes.width, 18.0f}, hint, 11.0f,
                         palette::withAlpha(t.textDim, 0.9f));
            break;
        }
    }
    // Dragging a handle writes the block's Start/Finish; the handles snap to
    // video frames so a slice boundary lands on a rendered frame.
    if (state.sliceDragNode != 0 && !ui::inputBlocked() &&
        IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        if (Node *node = project.graph.find(state.sliceDragNode)) {
            const double fps = std::max(1.0, project.video.fps);
            double value = xToTime(static_cast<float>(GetMouseX()) - state.sliceDragGrab);
            value = std::round(value * fps) / fps;
            node->setFloat(state.sliceDragEnd == 0 ? "start" : "finish",
                           static_cast<float>(value));
            project.dirty = true;
        } else {
            state.sliceDragNode = 0;
        }
    }
    if (ui::inputBlocked() || IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        state.sliceDragNode = 0;
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
