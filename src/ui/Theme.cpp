#include "ui/Theme.h"

#include <cstring>
#include <vector>

#include "render/Palette.h"

namespace pf::ui {

namespace {
Theme gTheme{};
CguiTheme *gDark = nullptr;
CguiTheme *gLight = nullptr;
bool gDarkTheme = true;
bool gKeyboardCaptured = false;
bool gInitialised = false;

void applyFonts(const CguiCrystallineThemeData *data) {
    if (!data) return;
    gTheme.heading = data->textFontBold.texture.id ? data->textFontBold : GetFontDefault();
    gTheme.body = data->textFont.texture.id ? data->textFont : GetFontDefault();
    gTheme.caption = data->textFontLight.texture.id ? data->textFontLight : gTheme.body;
    gTheme.headingSize = data->headingFontSize > 0 ? data->headingFontSize : 22.0f;
    gTheme.bodySize = data->bodyFontSize > 0 ? data->bodyFontSize : 16.0f;
    gTheme.captionSize = data->captionFontSize > 0 ? data->captionFontSize : 13.0f;
}

void applyColors(const CguiCrystallineThemeData *data) {
    // The widget colours are derived from the active CrystalGUI theme, so
    // switching between the dark and light crystalline themes restyles the
    // whole editor, not just the CrystalGUI components.
    if (data) {
        auto sla = [](Vector3 value, float hue) {
            return CguiColorFromHSLA(hue, value.x, value.y, value.z);
        };
        gTheme.background = sla(data->backgroundSLA, 0.0f);
        gTheme.panel = sla(data->backlayerSLA, 0.0f);
        gTheme.panelAlt = sla(data->midlayerSLA, 0.0f);
        gTheme.panelRaised = sla(data->frontlayerSLA, 0.0f);
        gTheme.border = sla(data->layerBorderSLA, 0.0f);
        // Text and accent are derived from the background luminance instead of
        // the theme's raw SLA values, which are not contrast-safe in the light
        // variant (dim text ended up lighter than the surface).
        const float luminance =
            (0.299f * gTheme.background.r + 0.587f * gTheme.background.g +
             0.114f * gTheme.background.b) / 255.0f;
        const bool light = luminance > 0.5f;
        const float textLightness = light ? 0.16f : 0.93f;
        const float dimLightness = light ? 0.42f : 0.62f;
        const float accentLightness = light ? 0.42f : 0.68f;
        gTheme.text = CguiColorFromHSLA(0.0f, 0.0f, textLightness, 1.0f);
        gTheme.textDim = CguiColorFromHSLA(0.0f, 0.08f, dimLightness, 1.0f);
        gTheme.accent = CguiColorFromHSLA(data->accentHue, 0.72f, accentLightness, 1.0f);
        gTheme.accentDim = CguiColorFromHSLA(data->accentHue, 0.55f, accentLightness + 0.12f,
                                             light ? 0.35f : 1.0f);
        gTheme.accentAlt = CguiColorFromHSLA(data->infoHue, 0.62f, accentLightness, 1.0f);
        gTheme.warn = CguiColorFromHSLA(data->warningHue, data->activeSLA.x, data->activeSLA.y,
                                        data->activeSLA.z);
        gTheme.warn = CguiColorFromHSLA(data->warningHue, 0.85f, light ? 0.42f : 0.62f, 1.0f);
        gTheme.danger = CguiColorFromHSLA(data->dangerousHue, 0.80f, light ? 0.45f : 0.62f, 1.0f);
        gTheme.success = CguiColorFromHSLA(data->successHue, data->activeSLA.x, data->activeSLA.y,
                                           data->activeSLA.z);
        gTheme.success = CguiColorFromHSLA(data->successHue, 0.70f, light ? 0.38f : 0.62f, 1.0f);
        gTheme.controlFill = CguiColorFromHSLA(0.0f, 0.0f, data->inactiveSLA.y, data->inactiveSLA.z);
        gTheme.controlFillFlat = CguiColorFromHSLA(0.0f, 0.0f, data->flatSLA.y, data->flatSLA.z);
        gTheme.controlBorder =
            CguiColorFromHSLA(0.0f, 0.0f, data->componentBorderSLA.y, data->componentBorderSLA.z);
        return;
    }
    gTheme.background = palette::background();
    gTheme.panel = palette::backlayer();
    gTheme.panelAlt = palette::midlayer();
    gTheme.panelRaised = palette::frontlayer();
    gTheme.border = palette::border();
    gTheme.text = palette::text();
    gTheme.textDim = palette::textDim();
    gTheme.accent = palette::accent();
    gTheme.accentDim = palette::modulate(palette::accent(), 0.55f);
    gTheme.accentAlt = palette::accentAlt();
    gTheme.warn = palette::warn();
    gTheme.danger = palette::danger();
    gTheme.success = palette::success();
    gTheme.controlFill = palette::frontlayer();
    gTheme.controlFillFlat = palette::midlayer();
    gTheme.controlBorder = palette::border();
}

}  // namespace
Color readableOn(Color fill, float alpha) {
    const float luminance =
        (0.299f * fill.r + 0.587f * fill.g + 0.114f * fill.b) / 255.0f;
    const float lightness = luminance > 0.55f ? 0.10f : 0.97f;
    return CguiColorFromHSLA(0.0f, 0.0f, lightness, alpha);
}


void init(bool dark) {
    if (gInitialised) return;
    // CrystalGUI's font loading warns about a few tall glyphs; they are harmless
    // and would otherwise flood the log once per character.
    SetTraceLogLevel(LOG_ERROR);
    CguiInit();
    // CguiInit() installs its own crystalline dark theme; keep a light one
    // around so the editor can switch palettes at runtime.
    gDark = CguiCreateCrystallineThemeDark();
    if (!gDark) gDark = CguiGetActiveTheme();
    gLight = CguiCreateCrystallineThemeLight();
    gDarkTheme = dark;
    if (dark) {
        CguiSetActiveTheme(gDark);
    } else if (gLight) {
        CguiSetActiveTheme(gLight);
    }
    const CguiTheme *active = CguiGetActiveTheme();
    const CguiCrystallineThemeData *data =
        active ? static_cast<const CguiCrystallineThemeData *>(active->themeData) : nullptr;
    applyFonts(data);
    applyColors(data);
    if (gTheme.body.texture.id == 0) gTheme.body = GetFontDefault();
    if (gTheme.heading.texture.id == 0) gTheme.heading = gTheme.body;
    if (gTheme.caption.texture.id == 0) gTheme.caption = gTheme.body;

    // One atlas per size class, so glyphs are always drawn at (or very near)
    // their native rasterisation size instead of being scaled, which is what
    // made the text look pixelated and broke thin strokes.
    const char *regular = "resource/fonts/Inter/static/Inter_18pt-Regular.ttf";
    const char *bold = "resource/fonts/Inter/static/Inter_24pt-SemiBold.ttf";
    // Latin-1 plus the typographic symbols the UI uses, so characters such as
    // x, /, minus, <=, >=, pi and arrows render instead of falling back.
    static std::vector<int> codepoints;
    if (codepoints.empty()) {
        for (int c = 32; c <= 126; ++c) codepoints.push_back(c);
        for (int c = 160; c <= 255; ++c) codepoints.push_back(c);
        const int extras[] = {0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                              0x2026, 0x2032, 0x2033, 0x2212, 0x221A, 0x221E, 0x2260,
                              0x2264, 0x2265, 0x03C0, 0x03A3, 0x2192, 0x2190, 0x2191,
                              0x2193, 0x25B6, 0x25A0};
        for (int c : extras) codepoints.push_back(c);
    }
    static const float kSizes[Theme::kAtlasCount] = {12.0f, 15.0f, 18.0f,
                                                    22.0f, 27.0f, 34.0f, 44.0f};
    for (int i = 0; i < Theme::kAtlasCount; ++i) {
        gTheme.atlasSizes[i] = kSizes[i];
        if (FileExists(regular)) {
            gTheme.regularAtlas[i] = LoadFontEx(regular, static_cast<int>(kSizes[i]),
                                                codepoints.data(),
                                                static_cast<int>(codepoints.size()));
        }
        if (FileExists(bold)) {
            gTheme.boldAtlas[i] =
                LoadFontEx(bold, static_cast<int>(kSizes[i]), codepoints.data(),
                           static_cast<int>(codepoints.size()));
        }
        // Bilinear only: mipmapping text makes small glyphs mushy because a
        // down-scaled atlas averages strokes away.
        if (gTheme.regularAtlas[i].texture.id != 0) {
            SetTextureFilter(gTheme.regularAtlas[i].texture, TEXTURE_FILTER_BILINEAR);
        }
        if (gTheme.boldAtlas[i].texture.id != 0) {
            SetTextureFilter(gTheme.boldAtlas[i].texture, TEXTURE_FILTER_BILINEAR);
        }
    }
    SetTraceLogLevel(LOG_WARNING);
    gInitialised = true;
}

const Font &Theme::font(float size, bool bold) const {
    const Font *table = bold ? boldAtlas : regularAtlas;
    for (int i = 0; i < Theme::kAtlasCount; ++i) {
        if (atlasSizes[i] + 0.5f >= size && table[i].texture.id != 0) return table[i];
    }
    // Larger than the biggest atlas (deep canvas zoom): use it anyway.
    for (int i = Theme::kAtlasCount - 1; i >= 0; --i) {
        if (table[i].texture.id != 0) return table[i];
    }
    return bold ? heading : body;
}

void shutdown() {
    if (!gInitialised) return;
    for (int i = 0; i < Theme::kAtlasCount; ++i) {
        if (gTheme.regularAtlas[i].texture.id != 0) UnloadFont(gTheme.regularAtlas[i]);
        if (gTheme.boldAtlas[i].texture.id != 0) UnloadFont(gTheme.boldAtlas[i]);
        gTheme.regularAtlas[i] = Font{};
        gTheme.boldAtlas[i] = Font{};
    }
    if (gLight) {
        CguiDeleteCrystallineTheme(gLight);
        gLight = nullptr;
    }
    CguiClose();
    gInitialised = false;
}

void setDarkTheme(bool dark) {
    gDarkTheme = dark;
    if (dark) {
        CguiSetActiveTheme(gDark);
    } else if (gLight) {
        CguiSetActiveTheme(gLight);
    }
    // Re-derive the widget palette from the newly active theme.
    const CguiTheme *active = CguiGetActiveTheme();
    applyColors(active ? static_cast<const CguiCrystallineThemeData *>(active->themeData) : nullptr);
}

bool isDarkTheme() { return gDarkTheme; }

Theme &theme() { return gTheme; }

bool keyboardCaptured() { return gKeyboardCaptured; }
void setKeyboardCaptured(bool captured) { gKeyboardCaptured = captured; }

}  // namespace pf::ui
