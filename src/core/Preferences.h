// Persisted application preferences, including user-defined theme palettes.
#pragma once

#include <string>
#include <vector>

#include "raylib.h"

namespace pf {

struct ThemePalette {
    std::string name;
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
    Color controlFill{};
    Color controlFillFlat{};
    Color controlBorder{};
    Color warn{};
    Color danger{};
    Color success{};
};

// Application preferences persisted next to the executable.
struct Preferences {
    bool defaultDarkTheme = true;
    // Name of the selected built-in or user palette. Built-ins are "Dark",
    // "Light" and "Immaterial"; user names are keys in `themePalettes`.
    std::string themePalette = "Dark";
    // User palettes only. They are written as a JSON object keyed by name.
    std::vector<ThemePalette> themePalettes;
    float guiScale = 1.25f;
    // Decode and analyse imported audio on a worker thread, so opening a project
    // or dropping a file does not block the editor; the timeline and the
    // preview stay greyed out until the clip is ready.
    bool asyncAudioLoad = true;
    // Editor frame rate cap in frames per second; 0 draws as fast as the work
    // allows. The editor redraws everything every frame (immediate mode), so
    // this is the knob that decides how much CPU and GPU it may spend.
    int editorFpsCap = 60;
};

void loadPreferences(Preferences &preferences, const std::string &path);
bool savePreferences(const Preferences &preferences, const std::string &path);

}  // namespace pf
