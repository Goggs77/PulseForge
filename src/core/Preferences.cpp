#include "core/Preferences.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <utility>

#include "core/Json.h"
#include "render/Palette.h"

namespace pf {

namespace {

json::Value colorToJson(Color color) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X%02X", color.r, color.g, color.b,
                  color.a);
    return json::Value::makeString(buffer);
}

Color colorFromJson(const json::Value &value, Color fallback) {
    const std::string text = value.asString();
    if (text.size() < 7 || text[0] != '#') return fallback;
    unsigned int components[4] = {0, 0, 0, 255};
    const size_t count = std::min<size_t>(4, (text.size() - 1) / 2);
    for (size_t i = 0; i < count; ++i) {
        components[i] = static_cast<unsigned int>(
            std::strtoul(text.substr(1 + i * 2, 2).c_str(), nullptr, 16));
    }
    return Color{static_cast<unsigned char>(components[0]),
                 static_cast<unsigned char>(components[1]),
                 static_cast<unsigned char>(components[2]),
                 static_cast<unsigned char>(count >= 4 ? components[3] : 255)};
}

json::Value paletteToJson(const ThemePalette &palette) {
    json::Value value = json::Value::makeObject();
    value.set("background", colorToJson(palette.background));
    value.set("panel", colorToJson(palette.panel));
    value.set("panelAlt", colorToJson(palette.panelAlt));
    value.set("panelRaised", colorToJson(palette.panelRaised));
    value.set("border", colorToJson(palette.border));
    value.set("text", colorToJson(palette.text));
    value.set("textDim", colorToJson(palette.textDim));
    value.set("accent", colorToJson(palette.accent));
    value.set("accentDim", colorToJson(palette.accentDim));
    value.set("accentAlt", colorToJson(palette.accentAlt));
    value.set("controlFill", colorToJson(palette.controlFill));
    value.set("controlFillFlat", colorToJson(palette.controlFillFlat));
    value.set("controlBorder", colorToJson(palette.controlBorder));
    value.set("warn", colorToJson(palette.warn));
    value.set("danger", colorToJson(palette.danger));
    value.set("success", colorToJson(palette.success));
    return value;
}

ThemePalette paletteFromJson(const std::string &name, const json::Value &value) {
    ThemePalette palette;
    palette.name = name;
    palette.background = colorFromJson(value["background"], palette::background());
    palette.panel = colorFromJson(value["panel"], palette::backlayer());
    palette.panelAlt = colorFromJson(value["panelAlt"], palette::midlayer());
    palette.panelRaised = colorFromJson(value["panelRaised"], palette::frontlayer());
    palette.border = colorFromJson(value["border"], palette::border());
    palette.text = colorFromJson(value["text"], palette::text());
    palette.textDim = colorFromJson(value["textDim"], palette::textDim());
    palette.accent = colorFromJson(value["accent"], palette::accent());
    palette.accentDim = colorFromJson(value["accentDim"], palette::accent());
    palette.accentAlt = colorFromJson(value["accentAlt"], palette::accentAlt());
    palette.controlFill = colorFromJson(value["controlFill"], palette::frontlayer());
    palette.controlFillFlat = colorFromJson(value["controlFillFlat"], palette::midlayer());
    palette.controlBorder = colorFromJson(value["controlBorder"], palette::border());
    palette.warn = colorFromJson(value["warn"], palette::warn());
    palette.danger = colorFromJson(value["danger"], palette::danger());
    palette.success = colorFromJson(value["success"], palette::success());
    return palette;
}

}  // namespace

void loadPreferences(Preferences &preferences, const std::string &path) {
    Preferences loaded;
    std::ifstream file(path.c_str(), std::ios::binary);
    if (!file.good()) {
        preferences = std::move(loaded);
        return;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    json::Value root;
    if (!json::parse(buffer.str(), root, nullptr)) {
        preferences = std::move(loaded);
        return;
    }

    loaded.defaultDarkTheme = root["defaultDarkTheme"].asBool(true);
    // Older files only stored a dark/light boolean. Preserve that choice while
    // moving to named palettes; a file with neither field uses the new default.
    const std::string legacyPalette = loaded.defaultDarkTheme ? "Dark" : "Light";
    loaded.themePalette = root["themePalette"].asString(
        root.has("defaultDarkTheme") ? legacyPalette : "Immaterial");
    loaded.guiScale = std::clamp(root["guiScale"].asFloat(1.25f), 0.75f, 2.0f);
    loaded.asyncAudioLoad = root["asyncAudioLoad"].asBool(true);
    loaded.editorFpsCap = std::clamp(root["editorFpsCap"].asInt(60), 0, 240);
    loaded.snapBlocksToGrid = root["snapBlocksToGrid"].asBool(true);

    const json::Value &palettes = root["themePalettes"];
    if (palettes.isObject()) {
        for (const auto &entry : palettes.object) {
            if (entry.first.empty() || !entry.second.isObject()) continue;
            loaded.themePalettes.push_back(paletteFromJson(entry.first, entry.second));
        }
    }
    preferences = std::move(loaded);
}

bool savePreferences(const Preferences &preferences, const std::string &path) {
    json::Value root = json::Value::makeObject();
    root.set("application", "PulseForge");
    root.set("defaultDarkTheme", preferences.defaultDarkTheme);
    root.set("themePalette", preferences.themePalette);
    json::Value palettes = json::Value::makeObject();
    for (const ThemePalette &palette : preferences.themePalettes) {
        if (!palette.name.empty()) palettes.set(palette.name, paletteToJson(palette));
    }
    root.set("themePalettes", palettes);
    root.set("guiScale", preferences.guiScale);
    root.set("asyncAudioLoad", preferences.asyncAudioLoad);
    root.set("editorFpsCap", preferences.editorFpsCap);
    root.set("snapBlocksToGrid", preferences.snapBlocksToGrid);
    std::ofstream file(path.c_str(), std::ios::binary);
    if (!file.good()) return false;
    file << json::write(root, 2);
    return true;
}

}  // namespace pf
