// CrystalGUI theme integration plus the shared look used by the PulseForge
// immediate-mode widgets.
#pragma once

#include <string>

#include "crystalgui/crystalgui.h"
#include "raylib.h"

namespace pf::ui {

struct Theme {
    Font body{};
    Font heading{};
    Font caption{};
    // Several atlases per family: text is always drawn at (or near) its native
    // size, which keeps glyphs crisp instead of scaling a small atlas up. The
    // sizes are close together so the worst-case scale factor stays small.
    // The first kBaseAtlasCount are built at startup; the larger ones (for the
    // Self Reference's export hyper resolution and a big GUI scale) load on
    // demand through loadLargeFonts().
    static constexpr int kAtlasCount = 10;
    static constexpr int kBaseAtlasCount = 7;
    Font regularAtlas[kAtlasCount]{};
    Font boldAtlas[kAtlasCount]{};
    float atlasSizes[kAtlasCount]{12.0f, 15.0f, 18.0f, 22.0f, 27.0f, 34.0f, 44.0f,
                                  54.0f, 68.0f, 88.0f};

    // Picks the smallest atlas that is at least `size` pixels tall.
    const Font &font(float size, bool bold = false) const;
    // The TTFs the atlases above are built from; the renderer loads them at the
    // larger sizes a Textbox block asks for instead of upscaling an atlas.
    std::string regularFontPath;
    std::string boldFontPath;
    float bodySize = 16.0f;
    float headingSize = 22.0f;
    float captionSize = 13.0f;
    // GUI scaling factor from the preferences; every widget metric and label
    // size is multiplied by it.
    float uiScale = 1.25f;
    // Extra factor for off-screen passes that draw the editor above the window
    // resolution (the Self Reference block's export hyper resolution). Metrics,
    // text and the font atlases all go through it, so a pass that draws into
    // (window size x renderScale) pixels has exactly the on-screen layout with
    // that many more pixels. Always 1 in normal frames.
    float renderScale = 1.0f;

    Color background{};
    Color panel{};
    Color panelAlt{};
    Color panelRaised{};
    Color border{};
    Color text{};
    Color textDim{};
    Color accent{};
    Color accentDim{};
    Color accentAlt{};
    // Fill colours CrystalGUI's button templates use, so labels drawn by the app
    // can pick ink that contrasts with the actual button behind them.
    Color controlFill{};
    Color controlFillFlat{};
    Color controlBorder{};
    Color warn{};
    Color danger{};
    Color success{};
};

// Calls CguiInit() and adopts the crystalline theme's fonts and dimensions.
void init(bool dark = true);
// Builds the atlases larger than kBaseAtlasCount on demand (the hyper
// resolution pass and a large GUI scale). Safe to call every pass.
void loadLargeFonts();
void shutdown();
void setDarkTheme(bool dark);
bool isDarkTheme();
Theme &theme();

// Scales a logical pixel measurement by the GUI scaling factor.
inline float s(float value) { return value * theme().uiScale * theme().renderScale; }

// A measurement in the graph canvas's own coordinate space (block and group
// geometry). The canvas zoom maps world units to pixels and a render-scaled pass
// doubles that zoom, so world geometry follows the GUI scale alone - everything
// drawn inside a block after worldToScreen() is screen space and uses s().
inline float sWorld(float value) { return value * theme().uiScale; }

// The pixel size a text of `size` is drawn at in the current pass; the font
// atlas is picked with it, so glyphs are rasterised at (or just above) that
// size instead of being scaled up.
inline float textPixels(float size) {
    return size * theme().uiScale * theme().renderScale;
}

// Ink colour that stays readable on top of `fill` (used for block headers and
// filled buttons, whose colours come from the palette rather than the theme).
Color readableOn(Color fill, float alpha = 1.0f);

// True when a text field currently owns the keyboard.
bool keyboardCaptured();
void setKeyboardCaptured(bool captured);

}  // namespace pf::ui
