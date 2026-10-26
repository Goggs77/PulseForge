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
ThemePalette gDarkPalette;
ThemePalette gLightPalette;
ThemePalette gImmaterialPalette;
std::vector<ThemePalette> gBuiltinPalettes;

void applyFonts(const CguiCrystallineThemeData *data) {
    if (!data) return;
    gTheme.heading = data->textFontBold.texture.id ? data->textFontBold : GetFontDefault();
    gTheme.body = data->textFont.texture.id ? data->textFont : GetFontDefault();
    gTheme.caption = data->textFontLight.texture.id ? data->textFontLight : gTheme.body;
    gTheme.headingSize = data->headingFontSize > 0 ? data->headingFontSize : 22.0f;
    gTheme.bodySize = data->bodyFontSize > 0 ? data->bodyFontSize : 16.0f;
    gTheme.captionSize = data->captionFontSize > 0 ? data->captionFontSize : 13.0f;
}

ThemePalette paletteFromData(const CguiCrystallineThemeData *data) {
    ThemePalette result;
    if (data) {
        auto sla = [](Vector3 value, float hue) {
            return CguiColorFromHSLA(hue, value.x, value.y, value.z);
        };
        result.background = sla(data->backgroundSLA, 0.0f);
        result.panel = sla(data->backlayerSLA, 0.0f);
        result.panelAlt = sla(data->midlayerSLA, 0.0f);
        result.panelRaised = sla(data->frontlayerSLA, 0.0f);
        result.border = sla(data->layerBorderSLA, 0.0f);
        // Text and accent are derived from the background luminance instead of
        // the theme's raw SLA values, which are not contrast-safe in the light
        // variant (dim text ended up lighter than the surface).
        const float luminance =
            (0.299f * result.background.r + 0.587f * result.background.g +
             0.114f * result.background.b) / 255.0f;
        const bool light = luminance > 0.5f;
        const float textLightness = light ? 0.16f : 0.93f;
        const float dimLightness = light ? 0.42f : 0.62f;
        const float accentLightness = light ? 0.42f : 0.68f;
        result.text = CguiColorFromHSLA(0.0f, 0.0f, textLightness, 1.0f);
        result.textDim = CguiColorFromHSLA(0.0f, 0.08f, dimLightness, 1.0f);
        result.accent = CguiColorFromHSLA(data->accentHue, 0.72f, accentLightness, 1.0f);
        result.accentDim = CguiColorFromHSLA(data->accentHue, 0.55f, accentLightness + 0.12f,
                                             light ? 0.35f : 1.0f);
        result.accentAlt = CguiColorFromHSLA(data->infoHue, 0.62f, accentLightness, 1.0f);
        result.warn = CguiColorFromHSLA(data->warningHue, 0.85f, light ? 0.42f : 0.62f, 1.0f);
        result.danger =
            CguiColorFromHSLA(data->dangerousHue, 0.80f, light ? 0.45f : 0.62f, 1.0f);
        result.success =
            CguiColorFromHSLA(data->successHue, 0.70f, light ? 0.38f : 0.62f, 1.0f);
        result.controlFill =
            CguiColorFromHSLA(0.0f, 0.0f, data->inactiveSLA.y, data->inactiveSLA.z);
        result.controlFillFlat =
            CguiColorFromHSLA(0.0f, 0.0f, data->flatSLA.y, data->flatSLA.z);
        result.controlBorder =
            CguiColorFromHSLA(0.0f, 0.0f, data->componentBorderSLA.y, data->componentBorderSLA.z);
        return result;
    }
    result.background = palette::background();
    result.panel = palette::backlayer();
    result.panelAlt = palette::midlayer();
    result.panelRaised = palette::frontlayer();
    result.border = palette::border();
    result.text = palette::text();
    result.textDim = palette::textDim();
    result.accent = palette::accent();
    result.accentDim = palette::modulate(palette::accent(), 0.55f);
    result.accentAlt = palette::accentAlt();
    result.warn = palette::warn();
    result.danger = palette::danger();
    result.success = palette::success();
    result.controlFill = palette::frontlayer();
    result.controlFillFlat = palette::midlayer();
    result.controlBorder = palette::border();
    return result;
}

void copyPaletteColours(const ThemePalette &palette) {
    gTheme.background = palette.background;
    gTheme.panel = palette.panel;
    gTheme.panelAlt = palette.panelAlt;
    gTheme.panelRaised = palette.panelRaised;
    gTheme.border = palette.border;
    gTheme.text = palette.text;
    gTheme.textDim = palette.textDim;
    gTheme.accent = palette.accent;
    gTheme.accentDim = palette.accentDim;
    gTheme.accentAlt = palette.accentAlt;
    gTheme.controlFill = palette.controlFill;
    gTheme.controlFillFlat = palette.controlFillFlat;
    gTheme.controlBorder = palette.controlBorder;
    gTheme.warn = palette.warn;
    gTheme.danger = palette.danger;
    gTheme.success = palette.success;
}

const CguiCrystallineThemeData *themeData(CguiTheme *theme) {
    return theme ? static_cast<const CguiCrystallineThemeData *>(theme->themeData) : nullptr;
}

ThemePalette immaterialPalette() {
    ThemePalette palette;
    palette.name = "Immaterial";
    palette.background = pf::palette::fromHex(0x080B18);
    palette.panel = pf::palette::fromHex(0x10162A);
    palette.panelAlt = pf::palette::fromHex(0x18213C);
    palette.panelRaised = pf::palette::fromHex(0x243052);
    palette.border = pf::palette::fromHex(0x3D4E78);
    palette.text = pf::palette::fromHex(0xE8EEFF);
    palette.textDim = pf::palette::fromHex(0x91A1C6);
    palette.accent = pf::palette::fromHex(0x69E3FF);
    palette.accentDim = pf::palette::fromHex(0x347A96);
    palette.accentAlt = pf::palette::fromHex(0xC38DFF);
    palette.controlFill = pf::palette::fromHex(0x243052);
    palette.controlFillFlat = pf::palette::fromHex(0x18213C);
    palette.controlBorder = pf::palette::fromHex(0x4E6797);
    palette.warn = pf::palette::fromHex(0xF7C96B);
    palette.danger = pf::palette::fromHex(0xFF6B8A);
    palette.success = pf::palette::fromHex(0x73E6B0);
    return palette;
}

// Latin-1 plus the typographic symbols the UI uses, so characters such as x, /,
// minus, <=, >=, pi and arrows render instead of falling back.
// LoadFontEx takes a mutable pointer, so the shared list is handed out mutable.
std::vector<int> &glyphCodepoints() {
    static std::vector<int> codepoints = [] {
        std::vector<int> list;
        for (int c = 32; c <= 126; ++c) list.push_back(c);
        for (int c = 160; c <= 255; ++c) list.push_back(c);
        const int extras[] = {0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                              0x2026, 0x2032, 0x2033, 0x2212, 0x221A, 0x221E, 0x2260,
                              0x2264, 0x2265, 0x03C0, 0x03A3, 0x2192, 0x2190, 0x2191,
                              0x2193, 0x25B6, 0x25A0};
        for (int c : extras) list.push_back(c);
        return list;
    }();
    return codepoints;
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
    const CguiCrystallineThemeData *data = themeData(CguiGetActiveTheme());
    applyFonts(data);
    gDarkPalette = paletteFromData(themeData(gDark));
    gDarkPalette.name = "Dark";
    gLightPalette = paletteFromData(themeData(gLight));
    gLightPalette.name = "Light";
    gImmaterialPalette = immaterialPalette();
    gBuiltinPalettes = {gDarkPalette, gLightPalette, gImmaterialPalette};
    copyPaletteColours(dark ? gDarkPalette : gLightPalette);
    if (gTheme.body.texture.id == 0) gTheme.body = GetFontDefault();
    if (gTheme.heading.texture.id == 0) gTheme.heading = gTheme.body;
    if (gTheme.caption.texture.id == 0) gTheme.caption = gTheme.body;

    // One atlas per size class, so glyphs are always drawn at (or very near)
    // their native rasterisation size instead of being scaled, which is what
    // made the text look pixelated and broke thin strokes.
    const char *regular = "resource/fonts/Inter/static/Inter_18pt-Regular.ttf";
    const char *bold = "resource/fonts/Inter/static/Inter_24pt-SemiBold.ttf";
    gTheme.regularFontPath = regular;
    gTheme.boldFontPath = bold;
    std::vector<int> &codepoints = glyphCodepoints();
    static const float kSizes[Theme::kAtlasCount] = {12.0f, 15.0f, 18.0f,
                                                     22.0f, 27.0f, 34.0f, 44.0f, 54.0f, 68.0f, 88.0f};
    for (int i = 0; i < Theme::kBaseAtlasCount; ++i) {
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

void loadLargeFonts() {
    if (!gInitialised) return;
    std::vector<int> &codepoints = glyphCodepoints();
    for (int i = Theme::kBaseAtlasCount; i < Theme::kAtlasCount; ++i) {
        const int size = static_cast<int>(gTheme.atlasSizes[i]);
        if (gTheme.regularAtlas[i].texture.id == 0 &&
            FileExists(gTheme.regularFontPath.c_str())) {
            gTheme.regularAtlas[i] =
                LoadFontEx(gTheme.regularFontPath.c_str(), size, codepoints.data(),
                           static_cast<int>(codepoints.size()));
            if (gTheme.regularAtlas[i].texture.id != 0) {
                SetTextureFilter(gTheme.regularAtlas[i].texture, TEXTURE_FILTER_BILINEAR);
            }
        }
        if (gTheme.boldAtlas[i].texture.id == 0 && FileExists(gTheme.boldFontPath.c_str())) {
            gTheme.boldAtlas[i] =
                LoadFontEx(gTheme.boldFontPath.c_str(), size, codepoints.data(),
                           static_cast<int>(codepoints.size()));
            if (gTheme.boldAtlas[i].texture.id != 0) {
                SetTextureFilter(gTheme.boldAtlas[i].texture, TEXTURE_FILTER_BILINEAR);
            }
        }
    }
}

const Font &Theme::font(float size, bool bold) const {
    const Font *table = bold ? boldAtlas : regularAtlas;
    // The atlas has to match the pixel size the glyphs are drawn at, or the
    // rasterisation is scaled up (which is what made text look soft). The drawn
    // size is size * uiScale * renderScale.
    const float drawn = textPixels(size);
    for (int i = 0; i < Theme::kAtlasCount; ++i) {
        if (atlasSizes[i] + 0.5f >= drawn && table[i].texture.id != 0) return table[i];
    }
    // Larger than the biggest atlas (deep canvas zoom, a huge GUI scale): use it
    // anyway.
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
    if (dark != gDarkTheme) {
        if (dark && gDark) {
            CguiSetActiveTheme(gDark);
        } else if (!dark && gLight) {
            CguiSetActiveTheme(gLight);
        }
    }
    gDarkTheme = dark;
    copyPaletteColours(dark ? gDarkPalette : gLightPalette);
}

bool isDarkTheme() { return gDarkTheme; }

Theme &theme() { return gTheme; }

const std::vector<ThemePalette> &builtinThemePalettes() { return gBuiltinPalettes; }

ThemePalette currentThemePalette() {
    ThemePalette palette;
    palette.background = gTheme.background;
    palette.panel = gTheme.panel;
    palette.panelAlt = gTheme.panelAlt;
    palette.panelRaised = gTheme.panelRaised;
    palette.border = gTheme.border;
    palette.text = gTheme.text;
    palette.textDim = gTheme.textDim;
    palette.accent = gTheme.accent;
    palette.accentDim = gTheme.accentDim;
    palette.accentAlt = gTheme.accentAlt;
    palette.controlFill = gTheme.controlFill;
    palette.controlFillFlat = gTheme.controlFillFlat;
    palette.controlBorder = gTheme.controlBorder;
    palette.warn = gTheme.warn;
    palette.danger = gTheme.danger;
    palette.success = gTheme.success;
    return palette;
}

bool themePaletteIsDark(const ThemePalette &palette) {
    const float luminance =
        (0.299f * palette.background.r + 0.587f * palette.background.g +
         0.114f * palette.background.b) / 255.0f;
    return luminance <= 0.5f;
}

void applyThemePalette(const ThemePalette &palette) {
    const bool dark = themePaletteIsDark(palette);
    if (dark != gDarkTheme) {
        if (dark && gDark) {
            CguiSetActiveTheme(gDark);
        } else if (!dark && gLight) {
            CguiSetActiveTheme(gLight);
        }
    }
    gDarkTheme = dark;
    copyPaletteColours(palette);
}

bool keyboardCaptured() { return gKeyboardCaptured; }
void setKeyboardCaptured(bool captured) { gKeyboardCaptured = captured; }

}  // namespace pf::ui
