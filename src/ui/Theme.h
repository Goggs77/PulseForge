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
    static constexpr int kAtlasCount = 7;
    Font regularAtlas[kAtlasCount]{};
    Font boldAtlas[kAtlasCount]{};
    float atlasSizes[kAtlasCount]{12.0f, 15.0f, 18.0f, 22.0f, 27.0f, 34.0f, 44.0f};

    // Picks the smallest atlas that is at least `size` pixels tall.
    const Font &font(float size, bool bold = false) const;
    float bodySize = 16.0f;
    float headingSize = 22.0f;
    float captionSize = 13.0f;
    // GUI scaling factor from the preferences; every widget metric and label
    // size is multiplied by it.
    float uiScale = 1.25f;

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
void shutdown();
void setDarkTheme(bool dark);
bool isDarkTheme();
Theme &theme();

// Scales a logical pixel measurement by the GUI scaling factor.
inline float s(float value) { return value * theme().uiScale; }

// Ink colour that stays readable on top of `fill` (used for block headers and
// filled buttons, whose colours come from the palette rather than the theme).
Color readableOn(Color fill, float alpha = 1.0f);

// True when a text field currently owns the keyboard.
bool keyboardCaptured();
void setKeyboardCaptured(bool captured);

}  // namespace pf::ui
