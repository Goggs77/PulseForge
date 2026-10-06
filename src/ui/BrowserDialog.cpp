// File browser, export dialog and help overlay.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <string>

#include "export/FFmpeg.h"
#include "export/Exporter.h"
#include "core/Json.h"
#include "render/Palette.h"
#include "ui/App.h"

namespace fs = std::filesystem;

namespace pf {

void loadPreferences(Preferences &preferences, const std::string &path) {
    std::ifstream file(path.c_str(), std::ios::binary);
    if (!file.good()) return;
    std::stringstream buffer;
    buffer << file.rdbuf();
    json::Value root;
    if (!json::parse(buffer.str(), root, nullptr)) return;
    preferences.defaultDarkTheme = root["defaultDarkTheme"].asBool(true);
    preferences.guiScale = std::clamp(root["guiScale"].asFloat(1.25f), 0.75f, 2.0f);
}

bool savePreferences(const Preferences &preferences, const std::string &path) {
    json::Value root = json::Value::makeObject();
    root.set("application", "PulseForge");
    root.set("defaultDarkTheme", preferences.defaultDarkTheme);
    root.set("guiScale", preferences.guiScale);
    std::ofstream file(path.c_str(), std::ios::binary);
    if (!file.good()) return false;
    file << json::write(root, 2);
    return true;
}

void showMessage(UiState &state, const std::string &title, const std::string &text, bool error) {
    state.showMessage = true;
    state.messageTitle = title;
    state.messageText = text;
    state.messageIsError = error;
    state.messageConfirm = nullptr;
    TraceLog(error ? LOG_WARNING : LOG_INFO, "PulseForge: %s - %s", title.c_str(), text.c_str());
}

namespace {

// True when the render would deliver markedly less audio quality than the
// imported file, which is worth a reminder before spending render time.
bool exportDowngradesAudio(const UiState &state) {
    const Project &project = state.project;
    // A silent Audio Output has no track to compare the imported bitrate with.
    if (Exporter::audioRoute(project) == AudioRoute::Silent) return false;
    const double totalDuration = project.effectiveDuration(project.audio.duration);
    // The conversion is prepared on the first export, so the intent (a saved
    // project field) is what says whether the converted stream will be copied.
    const bool copiesConvertedStream =
        project.audio.transcodedAac && project.output.audioCodec == "aac" &&
        containerAcceptsAac(project.output.container) &&
        project.audio.duration + 0.05 >= totalDuration &&
        (project.output.audioSampleRate <= 0 || project.audio.transcodedRate <= 0 ||
         project.output.audioSampleRate == project.audio.transcodedRate);
    if (copiesConvertedStream) {
        return false;  // the converted stream goes through untouched
    }
    const int sourceKbps = static_cast<int>(project.audio.bitRate / 1000);
    const int targetKbps = parseAudioBitrateKbps(project.output.audioBitrate);
    if (sourceKbps <= 0 || targetKbps <= 0) return false;
    if (audioCodecIsLossless(project.audio.codec) ||
        audioCodecIsLossless(project.output.audioCodec)) {
        return false;
    }
    return targetKbps * 100 < sourceKbps * 80;
}

}  // namespace

void showConfirm(UiState &state, const std::string &title, const std::string &text, bool error,
                 std::function<void()> onConfirm) {
    state.showMessage = true;
    state.messageTitle = title;
    state.messageText = text;
    state.messageIsError = error;
    state.messageConfirm = std::move(onConfirm);
    TraceLog(error ? LOG_WARNING : LOG_INFO, "PulseForge: %s - %s", title.c_str(), text.c_str());
}

void requestExport(UiState &state) {
    Project &project = state.project;
    // A GPU encoder can be listed by ffmpeg but unusable on this machine (an
    // old card, a missing driver). Probe it once and offer the CPU fallback
    // instead of failing halfway through a render.
    const std::string codec = project.output.videoCodec;
    if (videoEncoderIsHardware(codec) && !ffmpeg::canRunVideoEncoder(codec)) {
        // Prefer a software encoder the container can take.
        std::string fallback = "libx264";
        for (const std::string &id : videoEncodersForContainer(project.output.container)) {
            if (!videoEncoderIsHardware(id) && ffmpeg::hasEncoder(id)) {
                fallback = id;
                break;
            }
        }
        char text[512];
        std::snprintf(text, sizeof(text),
                      "%s could not be initialised on this machine (no compatible GPU or "
                      "driver).\n\nExport with %s instead?",
                      videoEncoderLabel(codec).c_str(), videoEncoderLabel(fallback).c_str());
        showConfirm(state, "GPU encoder unavailable", text, true, [&state, fallback]() {
            state.project.output.videoCodec = fallback;
            state.project.dirty = true;
            requestExport(state);
        });
        return;
    }
    if (exportDowngradesAudio(state)) {
        const int sourceKbps = static_cast<int>(project.audio.bitRate / 1000);
        const int targetKbps = parseAudioBitrateKbps(project.output.audioBitrate);
        char text[512];
        std::snprintf(text, sizeof(text),
                      "The imported audio is %s at %d kbps, but the export is set to %s at "
                      "%d kbps (%d%% of the source).\n\n"
                      "Encoding at a much lower bitrate will be clearly audible. Raise 'Audio "
                      "kbps' in the Inspector, or continue anyway.",
                      audioCodecDisplayName(project.audio.codec).c_str(), sourceKbps,
                      audioCodecDisplayName(project.output.audioCodec).c_str(), targetKbps,
                      targetKbps * 100 / std::max(1, sourceKbps));
        showConfirm(state, "Audio quality warning", text, true, [&state]() {
            state.exporting = true;
            state.showExportDialog = false;
        });
        return;
    }
    state.exporting = true;
    state.showExportDialog = false;
}

namespace {

std::vector<std::string> splitExtensions(const std::string &filter) {
    std::vector<std::string> result;
    std::string current;
    for (char c : filter) {
        if (c == ',' || c == ';' || c == ' ') {
            if (!current.empty()) result.push_back(current);
            current.clear();
        } else if (c == '.') {
            current.clear();
            current.push_back('.');
        } else {
            current.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    if (!current.empty()) result.push_back(current);
    return result;
}

bool matchesFilter(const std::string &name, const std::vector<std::string> &extensions) {
    if (extensions.empty()) return true;
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const std::string &extension : extensions) {
        if (lower.size() >= extension.size() &&
            lower.compare(lower.size() - extension.size(), extension.size(), extension) == 0) {
            return true;
        }
    }
    return false;
}

std::string fileNameOf(const std::string &path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

}  // namespace

void refreshBrowserListing(BrowserState &browser) {
    browser.entryNames.clear();
    browser.entryIsDirectory.clear();
    browser.error.clear();
    if (browser.directory.empty()) {
        browser.directory = fs::current_path().string();
    }
    std::error_code error;
    fs::path directory(browser.directory);
    if (!fs::exists(directory, error) || !fs::is_directory(directory, error)) {
        browser.error = "Directory not found";
        browser.directory = fs::current_path().string();
        directory = fs::path(browser.directory);
    }
    const std::vector<std::string> extensions = splitExtensions(browser.filter);
    std::vector<std::string> directories;
    std::vector<std::string> files;
    for (fs::directory_iterator it(directory, fs::directory_options::skip_permission_denied, error), end;
         it != end && !error; it.increment(error)) {
        const std::string name = it->path().filename().string();
        if (!name.empty() && name[0] == '.') continue;
        std::error_code typeError;
        if (it->is_directory(typeError)) {
            directories.push_back(name);
        } else if (matchesFilter(name, extensions)) {
            files.push_back(name);
        }
    }
    std::sort(directories.begin(), directories.end());
    std::sort(files.begin(), files.end());
    for (const std::string &name : directories) {
        browser.entryNames.push_back(name);
        browser.entryIsDirectory.push_back(true);
    }
    for (const std::string &name : files) {
        browser.entryNames.push_back(name);
        browser.entryIsDirectory.push_back(false);
    }
    browser.scroll = 0.0f;
}

void openBrowser(UiState &state, const std::string &purpose, const std::string &title,
                 const std::string &filter, const std::string &initialPath) {
    state.browser.purpose = purpose;
    state.browser.title = title;
    state.browser.filter = filter;
    state.browser.open = true;
    state.browser.fileName = purpose == "project-save" ? "project.pforge" : std::string();

    std::string directory = initialPath;
    if (!directory.empty() && !fs::is_directory(directory)) {
        directory = Project::directoryOf(directory);
    }
    if (directory.empty() || !fs::is_directory(directory)) {
        directory = state.clip.valid() ? Project::directoryOf(state.clip.path()) : std::string();
    }
    if (directory.empty() || !fs::is_directory(directory)) {
        directory = fs::current_path().string();
    }
    state.browser.directory = directory;
    refreshBrowserListing(state.browser);
}

void drawBrowser(UiState &state) {
    if (!state.browser.open) return;
    const ui::Theme &t = ui::theme();
    BrowserState &browser = state.browser;

    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), palette::withAlpha(BLACK, 0.55f));
    const float width = std::min(760.0f, GetScreenWidth() - 40.0f);
    const float height = std::min(520.0f, GetScreenHeight() - 60.0f);
    const Rectangle box{(GetScreenWidth() - width) * 0.5f, (GetScreenHeight() - height) * 0.5f,
                        width, height};
    ui::panel(box, browser.title.c_str());
    DrawRectangleRoundedLines(box, 0.02f, 6, t.accent);

    // path + up
    const Rectangle pathBar{box.x + 10.0f, box.y + 32.0f, box.width - 130.0f, 24.0f};
    ui::drawTextClipped(Rectangle{pathBar.x, pathBar.y, pathBar.width, pathBar.height},
                        browser.directory.c_str(), 11.0f, t.textDim);
    if (ui::button(Rectangle{box.x + box.width - 112.0f, box.y + 32.0f, 50.0f, 24.0f}, "Up")) {
        const fs::path parent = fs::path(browser.directory).parent_path();
        if (!parent.empty()) {
            browser.directory = parent.string();
            refreshBrowserListing(browser);
        }
    }
    if (ui::button(Rectangle{box.x + box.width - 58.0f, box.y + 32.0f, 48.0f, 24.0f}, "Home")) {
        browser.directory = fs::current_path().string();
        refreshBrowserListing(browser);
    }

    const Rectangle list{box.x + 10.0f, box.y + 62.0f, box.width - 32.0f, height - 150.0f};
    DrawRectangleRounded(list, 0.02f, 4, palette::modulate(t.panelAlt, 0.85f));
    ui::beginScroll(list);
    const float rowHeight = 22.0f;
    const float contentHeight = rowHeight * static_cast<float>(browser.entryNames.size()) + 8.0f;
    if (ui::hovered(list)) ui::scrollWheel(&browser.scroll, contentHeight, list.height);
    for (size_t i = 0; i < browser.entryNames.size(); ++i) {
        const Rectangle row{list.x + 4.0f, list.y + 4.0f + rowHeight * static_cast<float>(i) - browser.scroll,
                            list.width - 12.0f, rowHeight};
        if (row.y + row.height < list.y || row.y > list.y + list.height) continue;
        const bool isHovered = ui::hovered(row) && ui::hovered(list);
        if (isHovered) DrawRectangleRounded(row, 0.3f, 4, palette::withAlpha(t.accent, 0.18f));
        ui::drawTextClipped(Rectangle{row.x + 6.0f, row.y, row.width - 12.0f, row.height},
                            ((browser.entryIsDirectory[i] ? "[ ] " : "    ") +
                             browser.entryNames[i])
                                .c_str(),
                            12.0f, browser.entryIsDirectory[i] ? t.accent : t.text);
        if (isHovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            if (browser.entryIsDirectory[i]) {
                browser.directory =
                    (fs::path(browser.directory) / browser.entryNames[i]).string();
                refreshBrowserListing(browser);
            } else {
                browser.fileName = browser.entryNames[i];
            }
        }
    }
    ui::endScroll();
    ui::scrollbar(Rectangle{list.x + list.width + 4.0f, list.y, 10.0f, list.height}, &browser.scroll,
                  contentHeight, list.height);

    if (!browser.error.empty()) {
        ui::drawText(Rectangle{list.x, list.y + list.height + 2.0f, list.width, 16.0f},
                     browser.error.c_str(), 11.0f, t.danger);
    }

    // file name
    const Rectangle nameBar{box.x + 10.0f, box.y + height - 74.0f, box.width - 220.0f, 26.0f};
    ui::textField(nameBar, &browser.fileName, "file name", 9001);
    char filterInfo[96];
    std::snprintf(filterInfo, sizeof(filterInfo), "filter: %s",
                  browser.filter.empty() ? "any" : browser.filter.c_str());
    ui::drawTextClipped(Rectangle{nameBar.x, nameBar.y + 28.0f, nameBar.width, 16.0f}, filterInfo,
                        10.0f, t.textDim);

    const Rectangle okBox{box.x + box.width - 200.0f, box.y + height - 74.0f, 90.0f, 26.0f};
    const Rectangle cancelBox{box.x + box.width - 104.0f, box.y + height - 74.0f, 94.0f, 26.0f};
    if (ui::button(okBox, "Select", true)) {
        std::string fullPath;
        if (!browser.fileName.empty()) {
            fullPath = (fs::path(browser.directory) / browser.fileName).string();
        }
        const std::string purpose = browser.purpose;
        browser.open = false;
        if (!fullPath.empty()) {
            if (purpose == "audio") {
                loadAudioFile(state, fullPath);
            } else if (purpose == "project-open") {
                loadProjectFile(state, fullPath);
            } else if (purpose == "project-save") {
                saveProjectFile(state, fullPath);
            } else if (purpose == "export") {
                state.exportPath = fullPath;
            } else if (purpose == "shader") {
                if (Node *node = state.project.graph.find(state.selectedNode)) {
                    node->setText("shader", fullPath);
                    state.project.dirty = true;
                    setStatus(state, "Shader set to " + fileNameOf(fullPath));
                }
            } else if (purpose == "node-file") {
                // Any File parameter (a picture, a font, ...) picks its path
                // through the same dialog.
                if (Node *node = state.project.graph.find(state.selectedNode)) {
                    if (!browser.paramKey.empty()) {
                        node->setText(browser.paramKey, fullPath);
                        state.project.dirty = true;
                        setStatus(state, node->displayTitle() + ": " + fileNameOf(fullPath));
                    }
                }
            }
        }
    }
    // Escape first leaves the filename field; a second press closes the dialog.
    if (ui::button(cancelBox, "Cancel") ||
        (!ui::keyboardCaptured() && IsKeyPressed(KEY_ESCAPE))) {
        browser.open = false;
    }
}

void drawExportDialog(UiState &state) {
    if (!state.showExportDialog) return;
    const ui::Theme &t = ui::theme();
    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), palette::withAlpha(BLACK, 0.55f));
    const float width = std::min(620.0f, GetScreenWidth() - 40.0f);
    const float height = 330.0f;
    const Rectangle box{(GetScreenWidth() - width) * 0.5f, (GetScreenHeight() - height) * 0.5f,
                        width, height};
    ui::panel(box, "Export video");
    DrawRectangleRoundedLines(box, 0.02f, 6, t.accent);

    Project &project = state.project;
    const double duration = project.effectiveDuration(state.clip.duration());
    Rectangle cursor{box.x + 14.0f, box.y + 40.0f, box.width - 28.0f, 24.0f};

    ui::drawTextClipped(Rectangle{cursor.x, cursor.y, 110.0f, cursor.height}, "Output file", 12.0f,
                        t.textDim);
    const std::string placeholder = "output." + project.output.container;
    if (ui::textField(Rectangle{cursor.x + 110.0f, cursor.y, cursor.width - 210.0f, cursor.height},
                      &state.exportPath, placeholder.c_str(), 9002)) {
    }
    if (ui::button(Rectangle{cursor.x + cursor.width - 94.0f, cursor.y, 94.0f, cursor.height},
                   "Browse")) {
        openBrowser(state, "export", "Choose output file", "." + project.output.container,
                    Project::directoryOf(state.exportPath));
    }
    cursor.y += 32.0f;

    char summary[320];
    std::snprintf(summary, sizeof(summary),
                  "%dx%d  %.0f fps  %d frames  %.2f s  %s / %s  %d Hz  crf %d",
                  project.video.width, project.video.height, project.video.fps,
                  static_cast<int>(std::ceil(duration * project.video.fps)), duration,
                  videoEncoderLabel(project.output.videoCodec).c_str(),
                  audioEncoderLabel(project.output.audioCodec).c_str(),
                  project.output.audioSampleRate > 0 ? project.output.audioSampleRate : 48000,
                  project.output.crf);
    ui::drawTextClipped(Rectangle{cursor.x, cursor.y, cursor.width, 20.0f}, summary, 12.0f, t.text);
    cursor.y += 24.0f;

    if (state.clip.valid()) {
        const int sourceKbps = static_cast<int>(project.audio.bitRate / 1000);
        std::string audioDesc = "audio: " + state.clip.path();
        ui::drawTextClipped(Rectangle{cursor.x, cursor.y, cursor.width, 18.0f}, audioDesc.c_str(),
                            11.0f, t.textDim);
        audioDesc = "[";
        audioDesc += project.audio.codec.empty() ? "unknown" : project.audio.codec;
        if (sourceKbps > 0) audioDesc += " " + std::to_string(sourceKbps) + " kbps";
        if (project.audio.transcodedAac) audioDesc += ", converted to AAC on export";
        audioDesc += "]  ->  export ";
        audioDesc += audioCodecDisplayName(project.output.audioCodec);
        if (!project.output.audioBitrate.empty() &&
            !audioCodecIsLossless(project.output.audioCodec)) {
            audioDesc += " " + project.output.audioBitrate;
        }
        ui::drawTextClipped(Rectangle{cursor.x, cursor.y + 16.0f, cursor.width, 18.0f},
                            audioDesc.c_str(), 11.0f,
                            project.audio.transcodedAac ? t.warn : t.textDim);
    } else {
        ui::drawTextClipped(Rectangle{cursor.x, cursor.y, cursor.width, 20.0f},
                            "no audio loaded - the export will be video only", 11.0f, t.warn);
    }
    cursor.y += 44.0f;

    // quick output settings
    const std::vector<std::string> containers = supportedContainers();
    int index = 0;
    for (size_t i = 0; i < containers.size(); ++i) {
        if (containers[i] == project.output.container) index = static_cast<int>(i);
    }
    const int before = index;
    ui::dropdown(Rectangle{cursor.x, cursor.y, 130.0f, 24.0f}, &index, containers, 9003);
    if (index != before) {
        project.output = outputSpecForContainer(containers[static_cast<size_t>(index)]);
        refreshOutputAudio(state);
        // The file name follows the container so the export does not write a
        // WebM/AVI stream into an ".mp4" path.
        updateExportExtension(state);
    }
    int crf = project.output.crf;
    if (ui::intSlider(Rectangle{cursor.x + 140.0f, cursor.y, cursor.width - 140.0f, 24.0f},
                      "Quality (CRF)", &crf, 0, 51, 9004)) {
        project.output.crf = crf;
    }
    cursor.y += 32.0f;

    ui::drawTextClipped(Rectangle{cursor.x, cursor.y, cursor.width, 18.0f},
                        "The renderer pipes raw RGBA frames straight into ffmpeg over a pipe; no "
                        "intermediate image sequence is written.",
                        10.0f, palette::withAlpha(t.textDim, 0.9f));
    cursor.y += 24.0f;

    const Rectangle logBox{cursor.x, cursor.y, cursor.width, 60.0f};
    DrawRectangleRounded(logBox, 0.04f, 4, Color{12, 13, 18, 255});
    ui::drawTextClipped(Rectangle{logBox.x + 6.0f, logBox.y + 2.0f, logBox.width - 12.0f, 14.0f},
                        Exporter::describeCommand(
                            Exporter::buildCommand(project,
                                                   ExportRequest{state.exportPath, true, 0.0, 0.0},
                                                   static_cast<int>(std::ceil(duration * project.video.fps))))
                            .c_str(),
                        10.0f, palette::withAlpha(t.textDim, 0.95f));
    if (!state.exportError.empty()) {
        ui::drawTextClipped(Rectangle{logBox.x + 6.0f, logBox.y + 20.0f, logBox.width - 12.0f, 16.0f},
                            state.exportError.c_str(), 11.0f, t.danger);
    }
    cursor.y += 68.0f;

    if (state.exporting) {
        ui::progressBar(Rectangle{cursor.x, cursor.y, cursor.width, 18.0f}, state.exportProgress,
                        state.exportStatus.c_str());
        cursor.y += 26.0f;
    }

    const Rectangle exportBox{cursor.x, box.y + box.height - 40.0f, 140.0f, 28.0f};
    const Rectangle cancelBox{cursor.x + 148.0f, box.y + box.height - 40.0f, 100.0f, 28.0f};
    if (ui::button(exportBox, state.exporting ? "Exporting..." : "Start export", true,
                   !state.exporting)) {
        state.exportError.clear();
        state.exportLog.clear();
        state.exportProgress = 0.0f;
        requestExport(state);
    }
    if (ui::button(cancelBox, "Close")) state.showExportDialog = false;
    if (!ui::keyboardCaptured() && IsKeyPressed(KEY_ESCAPE)) state.showExportDialog = false;
}

// Preferences: the application defaults plus the project's own information and
// metadata (resolution, frame rate, duration and output format).
void drawPreferencesDialog(UiState &state) {
    if (!state.showPreferences) return;
    const ui::Theme &t = ui::theme();
    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), palette::withAlpha(BLACK, 0.55f));

    const float width = std::min(660.0f, GetScreenWidth() - ui::s(40.0f));
    const float height = std::min(660.0f, GetScreenHeight() - ui::s(40.0f));
    const Rectangle box{(GetScreenWidth() - width) * 0.5f, (GetScreenHeight() - height) * 0.5f,
                        width, height};
    ui::panel(box, "Preferences");
    DrawRectangleRoundedLines(box, 0.02f, 6, t.accent);

    Rectangle cursor{box.x + ui::s(18.0f), box.y + ui::s(38.0f), box.width - ui::s(36.0f),
                     ui::s(24.0f)};
    const float row = ui::s(24.0f);
    const float gap = ui::s(5.0f);

    // ---- appearance ------------------------------------------------------
    ui::sectionHeader(Rectangle{cursor.x, cursor.y, cursor.width, ui::s(18.0f)}, "Appearance");
    cursor.y += ui::s(24.0f);
    ui::drawTextClipped(Rectangle{cursor.x, cursor.y, ui::s(150.0f), row}, "Default theme",
                        12.0f, t.textDim);
    {
        const Rectangle darkBox{cursor.x + ui::s(150.0f), cursor.y, ui::s(84.0f), row};
        const Rectangle lightBox{darkBox.x + darkBox.width + gap, cursor.y, ui::s(84.0f), row};
        if (ui::toggleButton(darkBox, "Dark", state.preferences.defaultDarkTheme)) {
            state.preferences.defaultDarkTheme = true;
            ui::setDarkTheme(true);
        }
        if (ui::toggleButton(lightBox, "Light", !state.preferences.defaultDarkTheme)) {
            state.preferences.defaultDarkTheme = false;
            ui::setDarkTheme(false);
        }
    }
    cursor.y += row + gap;
    ui::drawTextClipped(Rectangle{cursor.x, cursor.y, cursor.width, ui::s(16.0f)},
                        "Applied at startup and saved with the other preferences.", 10.5f,
                        palette::withAlpha(t.textDim, 0.9f));
    cursor.y += ui::s(20.0f);

    ui::drawTextClipped(Rectangle{cursor.x, cursor.y, ui::s(150.0f), row}, "GUI scaling",
                        12.0f, t.textDim);
    {
        float scale = state.preferences.guiScale;
        if (ui::slider(Rectangle{cursor.x + ui::s(150.0f), cursor.y, cursor.width - ui::s(230.0f), row},
                       nullptr, &scale, 0.75f, 2.0f, 0.05f, "%.2fx", 7105)) {
            state.preferences.guiScale = scale;
            ui::theme().uiScale = scale;
        }
        char percent[32];
        std::snprintf(percent, sizeof(percent), "%.0f%%", state.preferences.guiScale * 100.0f);
        ui::drawText(Rectangle{cursor.x + cursor.width - ui::s(70.0f), cursor.y, ui::s(70.0f), row},
                     percent, 12.0f, t.text, ui::Align::Right);
    }
    cursor.y += row + gap;
    if (ui::button(Rectangle{cursor.x + ui::s(150.0f), cursor.y, ui::s(120.0f), row},
                   "Reset layout")) {
        state.preferences.guiScale = 1.25f;
        ui::theme().uiScale = 1.25f;
    }
    cursor.y += row + ui::s(14.0f);

    // ---- project information and metadata --------------------------------
    Project &p = state.project;
    ui::sectionHeader(Rectangle{cursor.x, cursor.y, cursor.width, ui::s(18.0f)},
                      "Project information and metadata");
    cursor.y += ui::s(24.0f);

    ui::drawTextClipped(Rectangle{cursor.x, cursor.y, ui::s(150.0f), row}, "Project name",
                        12.0f, t.textDim);
    ui::textField(Rectangle{cursor.x + ui::s(150.0f), cursor.y, cursor.width - ui::s(150.0f), row},
                  &p.name, nullptr, 7101);
    cursor.y += row + gap;

    ui::drawTextClipped(Rectangle{cursor.x, cursor.y, ui::s(150.0f), row}, "Target resolution",
                        12.0f, t.textDim);
    {
        const float fieldWidth = (cursor.width - ui::s(150.0f) - gap) * 0.5f;
        ui::intField(Rectangle{cursor.x + ui::s(150.0f), cursor.y, fieldWidth, row}, &p.video.width,
                     16, 7680, 7102);
        ui::intField(Rectangle{cursor.x + ui::s(150.0f) + fieldWidth + gap, cursor.y, fieldWidth, row},
                     &p.video.height, 16, 4320, 7103);
    }
    cursor.y += row + gap;

    ui::drawTextClipped(Rectangle{cursor.x, cursor.y, ui::s(150.0f), row}, "Target frame rate",
                        12.0f, t.textDim);
    {
        float fps = static_cast<float>(p.video.fps);
        if (ui::slider(Rectangle{cursor.x + ui::s(150.0f), cursor.y, cursor.width - ui::s(230.0f), row},
                       nullptr, &fps, 1.0f, 240.0f, 1.0f, "%.0f", 7104)) {
            p.video.fps = std::lround(fps);
            p.dirty = true;
        }
        char unit[32];
        std::snprintf(unit, sizeof(unit), "%.0f fps", p.video.fps);
        ui::drawText(Rectangle{cursor.x + cursor.width - ui::s(70.0f), cursor.y, ui::s(70.0f), row},
                     unit, 12.0f, t.text, ui::Align::Right);
    }
    cursor.y += row + gap;

    ui::drawTextClipped(Rectangle{cursor.x, cursor.y, ui::s(150.0f), row}, "Duration", 12.0f,
                        t.textDim);
    if (ui::checkbox(Rectangle{cursor.x + ui::s(150.0f), cursor.y, ui::s(210.0f), row},
                     "Follow the audio", &p.video.useAudioDuration)) {
        p.dirty = true;
    }
    {
        const Rectangle valueBox{cursor.x + ui::s(366.0f), cursor.y, cursor.width - ui::s(366.0f),
                                 row};
        if (p.video.useAudioDuration) {
            char text[64];
            std::snprintf(text, sizeof(text), "%.2f s", state.clip.duration());
            ui::drawText(valueBox, text, 12.0f, t.textDim, ui::Align::Right);
        } else {
            float duration = static_cast<float>(p.video.duration);
            if (ui::slider(valueBox, nullptr, &duration, 0.5f, 900.0f, 0.1f, "%.1f s", 7106)) {
                p.video.duration = duration;
                p.dirty = true;
            }
        }
    }
    cursor.y += row + gap;

    ui::drawTextClipped(Rectangle{cursor.x, cursor.y, ui::s(150.0f), row}, "Output format",
                        12.0f, t.textDim);
    {
        const std::vector<std::string> containers = supportedContainers();
        int index = 0;
        for (size_t i = 0; i < containers.size(); ++i) {
            if (containers[i] == p.output.container) index = static_cast<int>(i);
        }
        const int before = index;
        ui::dropdown(Rectangle{cursor.x + ui::s(150.0f), cursor.y, ui::s(150.0f), row}, &index,
                     containers, 7107);
        if (index != before) {
            OutputSpec spec = outputSpecForContainer(containers[static_cast<size_t>(index)]);
            spec.crf = p.output.crf;
            p.output = spec;
            refreshOutputAudio(state);
            updateExportExtension(state);
            p.dirty = true;
        }
        char codecs[96];
        std::snprintf(codecs, sizeof(codecs), "%s / %s", videoEncoderLabel(p.output.videoCodec).c_str(),
                      audioCodecDisplayName(p.output.audioCodec).c_str());
        ui::drawTextClipped(Rectangle{cursor.x + ui::s(310.0f), cursor.y, cursor.width - ui::s(310.0f),
                                      row},
                            codecs, 11.5f, t.textDim);
    }
    cursor.y += row + gap;

    ui::drawTextClipped(Rectangle{cursor.x, cursor.y, ui::s(150.0f), row}, "Quality (CRF)",
                        12.0f, t.textDim);
    ui::intSlider(Rectangle{cursor.x + ui::s(150.0f), cursor.y, cursor.width - ui::s(230.0f), row},
                  nullptr, &p.output.crf, 0, 51, 7108);
    cursor.y += row + gap;

    ui::drawTextClipped(Rectangle{cursor.x, cursor.y, ui::s(150.0f), row}, "Audio bitrate",
                        12.0f, t.textDim);
    ui::textField(Rectangle{cursor.x + ui::s(150.0f), cursor.y, ui::s(110.0f), row},
                  &p.output.audioBitrate, nullptr, 7109);
    ui::drawTextClipped(Rectangle{cursor.x + ui::s(272.0f), cursor.y, ui::s(80.0f), row},
                        "Encoder preset", 12.0f, t.textDim);
    ui::textField(Rectangle{cursor.x + ui::s(366.0f), cursor.y, cursor.width - ui::s(366.0f), row},
                  &p.output.preset, nullptr, 7110);
    cursor.y += row + gap;

    ui::drawTextClipped(Rectangle{cursor.x, cursor.y, ui::s(150.0f), row}, "Output sample rate",
                        12.0f, t.textDim);
    {
        const std::vector<std::string> rates = {"22050", "32000", "44100", "48000",
                                                "88200", "96000", "192000"};
        int index = 3;
        for (size_t i = 0; i < rates.size(); ++i) {
            if (std::atoi(rates[i].c_str()) == p.output.audioSampleRate) {
                index = static_cast<int>(i);
            }
        }
        const int before = index;
        ui::dropdown(Rectangle{cursor.x + ui::s(150.0f), cursor.y, ui::s(110.0f), row}, &index,
                     rates, 7111);
        if (index != before) {
            p.output.audioSampleRate = std::atoi(rates[static_cast<size_t>(index)].c_str());
            p.dirty = true;
        }
        char hint[128];
        std::snprintf(hint, sizeof(hint), "media is %d Hz", state.clip.sampleRate());
        ui::drawTextClipped(Rectangle{cursor.x + ui::s(272.0f), cursor.y, cursor.width - ui::s(272.0f),
                                      row},
                            state.clip.valid() ? hint : "48 kHz unless changed here", 11.5f,
                            t.textDim);
    }
    cursor.y += row + ui::s(16.0f);

    // ---- footer ----------------------------------------------------------
    const Rectangle saveBox{box.x + box.width - ui::s(240.0f), box.y + box.height - ui::s(42.0f),
                            ui::s(130.0f), ui::s(28.0f)};
    const Rectangle closeBox{box.x + box.width - ui::s(104.0f), box.y + box.height - ui::s(42.0f),
                             ui::s(94.0f), ui::s(28.0f)};
    if (ui::button(saveBox, "Save preferences", true)) {
        ui::theme().uiScale = state.preferences.guiScale;
        if (savePreferences(state.preferences, state.preferencesPath)) {
            setStatus(state, "Preferences saved");
        } else {
            showMessage(state, "Preferences", "Could not write " + state.preferencesPath, true);
        }
    }
    if (ui::button(closeBox, "Close") ||
        (!ui::keyboardCaptured() && IsKeyPressed(KEY_ESCAPE))) {
        state.showPreferences = false;
    }
    ui::drawTextClipped(Rectangle{box.x + ui::s(18.0f), box.y + box.height - ui::s(42.0f),
                                  box.width - ui::s(280.0f), ui::s(28.0f)},
                        ("Saved to " + state.preferencesPath).c_str(), 10.5f, t.textDim);
}

void drawMessageBox(UiState &state) {
    if (!state.showMessage) return;
    const ui::Theme &t = ui::theme();
    const bool confirm = static_cast<bool>(state.messageConfirm);
    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), palette::withAlpha(BLACK, 0.6f));
    const float width = std::min(560.0f, GetScreenWidth() - ui::s(40.0f));
    const float padding = ui::s(18.0f);
    const float bodyWidth = width - padding * 2.0f;
    // The box grows with the message instead of clipping it: these bodies are
    // several wrapped lines long and were drawn through CrystalGUI's Pro text,
    // which also rendered them too thin to read comfortably.
    const float bodyHeight =
        ui::textWrappedHeight(state.messageText.c_str(), 13.0f, bodyWidth);
    const float height = std::min(GetScreenHeight() - ui::s(40.0f),
                                  std::max(ui::s(confirm ? 170.0f : 150.0f),
                                           ui::s(36.0f) + bodyHeight + ui::s(48.0f)));
    const Rectangle box{(GetScreenWidth() - width) * 0.5f, (GetScreenHeight() - height) * 0.5f,
                        width, height};
    ui::panel(box, state.messageTitle.c_str());
    DrawRectangleRoundedLines(box, 0.02f, 6, state.messageIsError ? t.danger : t.accent);
    const Rectangle bodyRect{box.x + padding, box.y + ui::s(36.0f), bodyWidth,
                             height - ui::s(84.0f)};
    BeginScissorMode(static_cast<int>(bodyRect.x), static_cast<int>(bodyRect.y),
                     static_cast<int>(bodyRect.width), static_cast<int>(bodyRect.height));
    ui::drawTextWrapped(bodyRect, state.messageText.c_str(), 13.0f,
                        state.messageIsError ? t.danger : t.text);
    EndScissorMode();
    const Rectangle okBox{box.x + box.width - ui::s(110.0f), box.y + height - ui::s(40.0f),
                          ui::s(94.0f), ui::s(28.0f)};
    if (confirm) {
        const Rectangle cancelBox{okBox.x - ui::s(104.0f), okBox.y, ui::s(96.0f), ui::s(28.0f)};
        const bool accepted = ui::button(okBox, "Continue", true) || IsKeyPressed(KEY_ENTER) ||
                              IsKeyPressed(KEY_KP_ENTER);
        const bool rejected = ui::button(cancelBox, "Cancel") || IsKeyPressed(KEY_ESCAPE);
        if (accepted || rejected) {
            std::function<void()> callback;
            if (accepted) callback = std::move(state.messageConfirm);
            state.messageConfirm = nullptr;
            state.showMessage = false;
            if (callback) callback();
        }
        return;
    }
    if (ui::button(okBox, "OK", true) || IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) ||
        IsKeyPressed(KEY_ESCAPE)) {
        state.showMessage = false;
    }
}

void drawHelpOverlay(UiState &state) {
    if (!state.showHelp) return;
    const ui::Theme &t = ui::theme();
    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), palette::withAlpha(BLACK, 0.6f));
    const float width = std::min(760.0f, GetScreenWidth() - 40.0f);
    const float height = std::min(560.0f, GetScreenHeight() - 60.0f);
    const Rectangle box{(GetScreenWidth() - width) * 0.5f, (GetScreenHeight() - height) * 0.5f,
                        width, height};
    ui::panel(box, "PulseForge - how it works");
    DrawRectangleRoundedLines(box, 0.02f, 6, t.accent);

    // One wrapped paragraph instead of hand-broken lines: the overlay keeps its
    // layout at every GUI scale and long lines can no longer be clipped.
    static const char *helpText =
        "Blocks are connected by typed ports: Audio (amber), Analysis (green), Scalar (blue), "
        "Image (violet) and Colour (pink).\n"
        "\n"
        "Typical chain\n"
        "  Audio Source -> Spectrum Analyzer -> Frequency Band -> LFO / Automation -> Spectrum "
        "(built-in effect) -> Shader (.glsl file) -> Geometry -> Post FX -> Video Output\n"
        "\n"
        "Spectrum and Shader\n"
        "  Spectrum draws the analysis with a built-in effect; its Scale, Feedback and "
        "Colour inputs can be modulated. Shader applies a .glsl file to an image and grows "
        "exactly the input ports that file uses (uPrev, uInput2, uUser[0..7], uColorA/B, "
        "uVector2/3/4, uMatrix). The PulseForge preamble is prepended unless the file "
        "starts with #version; helpers: pfUv(), pfPrev(), pfFeedback(), pfSpectrum(), "
        "pfWave(), pfFbm(), uTime, uBass/uMid/uTreble/uLevel/uOnset.\n"
        "\n"
        "Shortcuts\n"
        "  Space play/pause     Ctrl+S save     Ctrl+O open     Ctrl+N new\n"
        "  Ctrl+E export        Ctrl+, preferences    F2 preferences\n"
        "  Del delete block     F5 reload shaders     H help\n"
        "  Drag an audio file or .pforge onto the window to load it.\n"
        "\n"
        "Export\n"
        "  Frames are rendered off-screen at the project resolution and piped to ffmpeg as "
        "raw RGBA, so any container/codec ffmpeg supports is available. The audio keeps the "
        "imported codec and bitrate whenever this ffmpeg build can encode them.";
    // The body shrinks to fit when the GUI scale makes it taller than the panel,
    // and scrolls if even the smallest size does not fit, so no line is ever
    // clipped away.
    const float bodyWidth = box.width - 36.0f;
    const Rectangle bodyView{box.x + 18.0f, box.y + 38.0f, bodyWidth, box.height - 88.0f};
    float bodySize = 12.5f;
    while (bodySize > 9.0f &&
           ui::textWrappedHeight(helpText, bodySize, bodyWidth) > bodyView.height) {
        bodySize -= 0.5f;
    }
    static float helpScroll = 0.0f;
    const float contentHeight = ui::textWrappedHeight(helpText, bodySize, bodyWidth);
    const float maxScroll = std::max(0.0f, contentHeight - bodyView.height);
    if (ui::hovered(bodyView) && GetMouseWheelMove() != 0.0f) {
        helpScroll -= GetMouseWheelMove() * ui::s(48.0f);
    }
    helpScroll = std::clamp(helpScroll, 0.0f, maxScroll);
    BeginScissorMode(static_cast<int>(bodyView.x), static_cast<int>(bodyView.y),
                     static_cast<int>(bodyView.width), static_cast<int>(bodyView.height));
    ui::drawTextWrapped(Rectangle{bodyView.x, bodyView.y - helpScroll, bodyWidth, 0.0f}, helpText,
                        bodySize, palette::withAlpha(t.text, 1.0f));
    EndScissorMode();
    if (maxScroll > 0.0f) {
        ui::scrollbar(Rectangle{bodyView.x + bodyView.width - ui::s(6.0f), bodyView.y,
                                ui::s(5.0f), bodyView.height},
                      &helpScroll, contentHeight, bodyView.height);
    }
    if (ui::button(Rectangle{box.x + box.width - 110.0f, box.y + box.height - 40.0f, 96.0f, 26.0f},
                   "Close")) {
        state.showHelp = false;
    }
    // Only Escape closes it: the H that opened the overlay is still "pressed"
    // during this frame and would immediately close it again.
    if (IsKeyPressed(KEY_ESCAPE)) state.showHelp = false;
}

}  // namespace pf
