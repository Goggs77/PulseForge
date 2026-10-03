// Preview viewport: renders the pipeline at the playhead and shows transport.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "render/Palette.h"
#include "ui/App.h"

namespace pf {

void drawPreview(UiState &state, Rectangle bounds) {
    const ui::Theme &t = ui::theme();
    ui::panel(bounds, "Preview");

    Project &project = state.project;
    const Rectangle view{bounds.x + 1.0f, bounds.y + 26.0f, bounds.width - 2.0f,
                         bounds.height - 27.0f};
    const float controlsHeight = ui::s(62.0f);
    const Rectangle imageArea{view.x, view.y, view.width, std::max(ui::s(20.0f), view.height - controlsHeight)};
    const Rectangle controls{view.x, view.y + imageArea.height, view.width, controlsHeight};

    DrawRectangleRounded(imageArea, 0.02f, 4, Color{8, 9, 12, 255});
    if (state.previewImage && state.previewImage->valid()) {
        const float aspect = static_cast<float>(state.previewImage->width) /
                             static_cast<float>(std::max(1, state.previewImage->height));
        float width = imageArea.width - 12.0f;
        float height = width / aspect;
        if (height > imageArea.height - 12.0f) {
            height = imageArea.height - 12.0f;
            width = height * aspect;
        }
        const Rectangle dest{imageArea.x + (imageArea.width - width) * 0.5f,
                             imageArea.y + (imageArea.height - height) * 0.5f, width, height};
        // The renderer keeps targets in the normalised convention, so a positive
        // source height displays the frame upright.
        DrawTexturePro(state.previewImage->texture.texture,
                       Rectangle{0, 0, static_cast<float>(state.previewImage->width),
                                 static_cast<float>(state.previewImage->height)},
                       dest, Vector2{0, 0}, 0.0f, WHITE);
        DrawRectangleRoundedLines(dest, 0.0f, 1, palette::withAlpha(t.border, 0.9f));
    } else {
        ui::drawText(imageArea, "No frame yet", 13.0f, palette::withAlpha(t.textDim, 0.9f),
                     ui::Align::Center);
    }

    // ---- transport --------------------------------------------------------
    const double duration = project.effectiveDuration(state.clip.duration());
    const float buttonWidth = ui::s(62.0f);
    const float y = controls.y + ui::s(6.0f);
    if (ui::toggleButton(Rectangle{controls.x + ui::s(8.0f), y, buttonWidth, ui::s(24.0f)},
                         state.playing ? "Pause" : "Play", state.playing)) {
        if (!state.playing) {
            startPlayback(state);
        } else {
            pausePlayback(state);
        }
    }
    if (ui::button(Rectangle{controls.x + ui::s(8.0f) + buttonWidth + ui::s(6.0f), y, buttonWidth, ui::s(24.0f)},
                   "Stop")) {
        stopPlayback(state);
    }
    if (ui::toggleButton(Rectangle{controls.x + ui::s(8.0f) + (buttonWidth + ui::s(6.0f)) * 2.0f, y, ui::s(58.0f), ui::s(24.0f)},
                       "Loop", state.loopPlayback)) {
        state.loopPlayback = !state.loopPlayback;
    }
    {
        const char *labels[] = {"Full", "Half", "Quarter"};
        const int next = (state.previewScaleIndex + 1) % 3;
        if (ui::button(Rectangle{controls.x + ui::s(8.0f) + (buttonWidth + ui::s(6.0f)) * 2.0f + ui::s(64.0f),
                                 y, ui::s(62.0f), ui::s(24.0f)},
                       labels[state.previewScaleIndex])) {
            state.previewScaleIndex = next;
        }
    }

    const Rectangle scrub{controls.x + ui::s(8.0f), y + ui::s(30.0f), controls.width - ui::s(16.0f), ui::s(16.0f)};
    DrawRectangleRounded(scrub, 0.5f, 8, palette::modulate(t.panelAlt, 0.9f));
    const float fraction = duration > 0.0
                               ? static_cast<float>(std::clamp(state.playhead / duration, 0.0, 1.0))
                               : 0.0f;
    if (fraction > 0.0f) {
        DrawRectangleRounded(Rectangle{scrub.x, scrub.y, scrub.width * fraction, scrub.height}, 0.5f,
                             8, t.accentDim);
    }
    DrawCircleV(Vector2{scrub.x + scrub.width * fraction, scrub.y + scrub.height * 0.5f},
                ui::s(6.0f), t.accent);
    if (!ui::inputBlocked() &&
        ui::hovered(Rectangle{scrub.x, scrub.y - ui::s(6.0f), scrub.width, scrub.height + ui::s(12.0f)}) &&
        IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        const float local = std::clamp((GetMouseX() - scrub.x) / std::max(1.0f, scrub.width), 0.0f, 1.0f);
        state.playhead = local * duration;
        state.clip.seek(state.playhead + project.video.trimStart);
    }

    // ---- stats ------------------------------------------------------------
    char stats[220];
    std::snprintf(stats, sizeof(stats),
                  "%dx%d @ %.0f fps   frame %d   render %.2f ms   %d passes   %d/%d targets   ui %.0f "
                  "fps",
                  project.video.width, project.video.height, project.video.fps,
                  static_cast<int>(state.playhead * project.video.fps), state.lastRenderMs,
                  state.shaderPasses, state.pooledTargets, state.renderer.stats().peakTargets,
                  state.lastUiFps);
    ui::drawTextClipped(Rectangle{imageArea.x + ui::s(8.0f), imageArea.y + imageArea.height - ui::s(18.0f),
                                  imageArea.width - ui::s(16.0f), ui::s(16.0f)},
                        stats, 10.0f, palette::withAlpha(t.textDim, 0.95f));
}

}  // namespace pf
