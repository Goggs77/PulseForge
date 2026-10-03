// Inspector: parameters of the selected block, or the project settings when
// nothing is selected. Automation curves are edited here, inside their block.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "core/Registry.h"
#include "dsp/Analysis.h"
#include "export/FFmpeg.h"
#include "render/Palette.h"
#include "ui/App.h"

namespace pf {

namespace {

float kRow() { return ui::s(24.0f); }
float kGap() { return ui::s(4.0f); }
float kLabelWidth() { return ui::s(118.0f); }
float kCurveHeight() { return ui::s(132.0f); }

float labelWidthFor(Rectangle cursor) { return cursor.width < ui::s(300.0f) ? ui::s(92.0f) : kLabelWidth(); }

// Clamps the scroll offset, handles the wheel and draws the scroll bar. Called
// from every exit point of drawInspector.
void finishInspectorScroll(UiState &state, Rectangle view, float cursorBottom) {
    const float top = view.y + ui::s(8.0f);
    const float usable = view.height - ui::s(16.0f);
    const float contentHeight = (cursorBottom + state.inspectorScroll) - top;
    const float maxScroll = std::max(0.0f, contentHeight - usable);
    if (ui::hovered(view) && GetMouseWheelMove() != 0.0f) {
        state.inspectorScroll =
            std::clamp(state.inspectorScroll - GetMouseWheelMove() * ui::s(48.0f), 0.0f, maxScroll);
    }
    state.inspectorScroll = std::clamp(state.inspectorScroll, 0.0f, maxScroll);
    if (maxScroll > 0.0f) {
        ui::scrollbar(Rectangle{view.x + view.width - ui::s(10.0f), view.y, ui::s(7.0f), usable},
                      &state.inspectorScroll, contentHeight, usable);
    }
}

void drawParam(UiState &state, Node &node, Param &param, Rectangle &cursor) {
    const ui::Theme &t = ui::theme();
    const float labelWidth = labelWidthFor(cursor);
    const Rectangle labelRect{cursor.x, cursor.y, labelWidth, kRow()};
    const Rectangle fieldRect{cursor.x + labelWidth, cursor.y, cursor.width - labelWidth, kRow()};

    switch (param.kind) {
        case ParamKind::Bool:
            if (ui::checkbox(Rectangle{cursor.x, cursor.y, cursor.width, kRow()}, param.label.c_str(),
                             &param.boolean)) {
                state.project.dirty = true;
            }
            cursor.y += kRow() + kGap();
            break;

        case ParamKind::Color: {
            ui::drawTextClipped(labelRect, param.label.c_str(), 12.0f, t.textDim);
            const int colorId = ui::widgetId(node.id, param.key.c_str());
            if (ui::colorField(fieldRect, &param.color, colorId)) state.project.dirty = true;
            cursor.y += kRow() + kGap();
            break;
        }

        case ParamKind::Enum: {
            ui::drawTextClipped(labelRect, param.label.c_str(), 12.0f, t.textDim);
            int index = param.intValue();
            ui::dropdown(fieldRect, &index, param.options, ui::widgetId(node.id, param.key.c_str()));
            if (index != param.intValue()) {
                param.setInt(index);
                state.project.dirty = true;
            }
            cursor.y += kRow() + kGap();
            break;
        }

        case ParamKind::File:
            ui::drawTextClipped(labelRect, param.label.c_str(), 12.0f, t.textDim);
            if (ui::textField(fieldRect, &param.text, param.hint.c_str(),
                               ui::widgetId(node.id, param.key.c_str()))) {
                state.project.dirty = true;
            }
            cursor.y += kRow() + kGap();
            break;

        case ParamKind::Text:
            ui::drawTextClipped(labelRect, param.label.c_str(), 12.0f, t.textDim);
            if (ui::textField(fieldRect, &param.text, nullptr,
                               ui::widgetId(node.id, param.key.c_str()))) {
                state.project.dirty = true;
            }
            cursor.y += kRow() + kGap();
            break;

        case ParamKind::Int: {
            ui::drawTextClipped(Rectangle{cursor.x, cursor.y, cursor.width, 14.0f},
                                param.label.c_str(), 12.0f, t.textDim);
            int value = param.intValue();
            if (ui::intField(Rectangle{cursor.x, cursor.y + 14.0f, cursor.width, kRow()},
                             &value, static_cast<int>(param.minValue),
                             static_cast<int>(param.maxValue),
                             ui::widgetId(node.id, param.key.c_str()))) {
                param.setInt(value);
                state.project.dirty = true;
            }
            cursor.y += kRow() + 18.0f;
            break;
        }

        case ParamKind::Curve: {
            ui::drawTextClipped(Rectangle{cursor.x, cursor.y, cursor.width, 16.0f},
                                param.label.c_str(), 12.0f, t.textDim);
            cursor.y += 18.0f;
            const bool bipolar = node.pbool("bipolar", false);
            const double duration = state.project.effectiveDuration(state.clip.duration());
            const double playhead01 =
                duration > 0.0 ? std::clamp(state.playhead / duration, 0.0, 1.0) : 0.0;
            const Rectangle editor{cursor.x, cursor.y, cursor.width, kCurveHeight()};
            if (ui::curveEditor(editor, &param, bipolar, playhead01,
                                ui::widgetId(node.id, param.key.c_str()))) {
                state.project.dirty = true;
            }
            cursor.y += kCurveHeight() + 6.0f;
            break;
        }

        case ParamKind::Matrix: {
            // A grid of numeric boxes: the diagonal identity is visible at a
            // glance, which sliders never made clear.
            const int columns = std::clamp(node.pint("size", 1) + 2, 2, 4);
            ui::drawTextClipped(Rectangle{cursor.x, cursor.y, cursor.width, 16.0f},
                                param.label.c_str(), 12.0f, t.textDim);
            cursor.y += 18.0f;
            const float gap = ui::s(4.0f);
            const float cellWidth =
                (cursor.width - gap * static_cast<float>(columns - 1)) / static_cast<float>(columns);
            const float cellHeight = ui::s(22.0f);
            for (int row = 0; row < columns; ++row) {
                for (int column = 0; column < columns; ++column) {
                    const size_t index = static_cast<size_t>(row) * 4u + static_cast<size_t>(column);
                    if (index >= param.values.size()) continue;
                    const Rectangle cell{cursor.x + static_cast<float>(column) * (cellWidth + gap),
                                         cursor.y + static_cast<float>(row) * (cellHeight + gap),
                                         cellWidth, cellHeight};
                    char cellKey[32];
                    std::snprintf(cellKey, sizeof(cellKey), "%s%d", param.key.c_str(), row * 4 + column);
                    if (ui::floatField(cell, &param.values[index], -1.0e6f, 1.0e6f, "%.3g",
                                       ui::widgetId(node.id, cellKey))) {
                        state.project.dirty = true;
                    }
                }
            }
            cursor.y += static_cast<float>(columns) * (cellHeight + gap) + ui::s(6.0f);
            break;
        }

        default:
            ui::drawTextClipped(Rectangle{cursor.x, cursor.y, cursor.width, 14.0f},
                                param.label.c_str(), 12.0f, t.textDim);
            // Logarithmic parameters are frequencies: the track follows the way
            // the ear (and the analysis) divides the spectrum, and the readout is
            // a whole number of Hz instead of scientific notation.
            const std::string defaultFormat = param.logarithmic ? "%.0f Hz" : "%.3g";
            const char *valueFormat =
                param.valueFormat.empty() ? defaultFormat.c_str() : param.valueFormat.c_str();
            // A modulated parameter follows the value the block actually used, so
            // an LFO visibly moves the handle; while the user drags, the base
            // value is shown instead so the handle stays under the cursor.
            const int sliderId = ui::widgetId(node.id, param.key.c_str());
            float effective = 0.0f;
            float displayed = param.value;
            if (!ui::sliderDragging(sliderId) && node.effectiveParam(param.key, &effective)) {
                displayed = std::clamp(effective, std::min(param.minValue, param.maxValue),
                                       std::max(param.minValue, param.maxValue));
            }
            if (ui::slider(Rectangle{cursor.x, cursor.y + 14.0f, cursor.width, kRow()}, nullptr,
                           &displayed, param.minValue, param.maxValue, param.step, valueFormat,
                           sliderId, param.logarithmic)) {
                param.value = displayed;
                state.project.dirty = true;
            }
            cursor.y += kRow() + 18.0f;
            break;
    }
}

void drawDescription(Rectangle &cursor, const std::string &description) {
    const ui::Theme &t = ui::theme();
    // Drawn with the app's own atlas-backed renderer rather than CrystalGUI's
    // Pro text, which rendered thin, broken strokes. The wrapped height is
    // measured instead of estimated, so longer descriptions (and the larger GUI
    // scales) can no longer be cut off.
    // Mostly the primary ink: the dim grey it used to use was the other half of
    // "too thin to see clearly", especially on the light theme.
    Color ink = palette::mix(t.textDim, t.text, 0.6f);
    ink.a = 255;
    const float height = ui::drawTextWrapped(
        Rectangle{cursor.x, cursor.y, cursor.width, 0.0f}, description.c_str(), 13.0f, ink);
    cursor.y += height + ui::s(8.0f);
}

}  // namespace

void drawInspector(UiState &state, Rectangle bounds) {
    const ui::Theme &t = ui::theme();
    ui::panel(bounds, "Inspector");

    const Rectangle view{bounds.x + 1.0f, bounds.y + 26.0f, bounds.width - 2.0f,
                         bounds.height - 27.0f};
    Node *node = state.selectedNode > 0 ? state.project.graph.find(state.selectedNode) : nullptr;

    ui::beginScroll(view);
    Rectangle cursor{view.x + 10.0f, view.y + 8.0f - state.inspectorScroll, view.width - 34.0f,
                     kRow()};

    if (!node) {
        Project &p = state.project;

        ui::sectionHeader(Rectangle{cursor.x, cursor.y, cursor.width, 18.0f}, "Project");
        cursor.y += 24.0f;
        ui::drawTextClipped(Rectangle{cursor.x, cursor.y, 60.0f, kRow()}, "Name", 12.0f, t.textDim);
        if (ui::textField(Rectangle{cursor.x + 60.0f, cursor.y, cursor.width - 60.0f, kRow()},
                          &p.name, nullptr, 8101)) {
            p.dirty = true;
        }
        cursor.y += kRow() + kGap();

        ui::sectionHeader(Rectangle{cursor.x, cursor.y, cursor.width, 18.0f}, "Video");
        cursor.y += 24.0f;
        ui::drawTextClipped(Rectangle{cursor.x, cursor.y, 60.0f, kRow()}, "Width", 12.0f, t.textDim);
        if (ui::intField(Rectangle{cursor.x + 60.0f, cursor.y, cursor.width - 60.0f, kRow()},
                         &p.video.width, 16, 7680, 8102)) {
            p.dirty = true;
        }
        cursor.y += kRow() + kGap();
        ui::drawTextClipped(Rectangle{cursor.x, cursor.y, 60.0f, kRow()}, "Height", 12.0f, t.textDim);
        if (ui::intField(Rectangle{cursor.x + 60.0f, cursor.y, cursor.width - 60.0f, kRow()},
                         &p.video.height, 16, 4320, 8103)) {
            p.dirty = true;
        }
        cursor.y += kRow() + kGap();

        {
            static const char *names[] = {"1280x720", "1920x1080", "2560x1440", "3840x2160",
                                          "1080x1920"};
            static const int widths[] = {1280, 1920, 2560, 3840, 1080};
            static const int heights[] = {720, 1080, 1440, 2160, 1920};
            const float presetWidth = (cursor.width - 4.0f * kGap()) / 5.0f;
            for (int i = 0; i < 5; ++i) {
                const Rectangle r{cursor.x + static_cast<float>(i) * (presetWidth + kGap()),
                                  cursor.y, presetWidth, 20.0f};
                if (ui::smallButton(r, names[i],
                                    p.video.width == widths[i] && p.video.height == heights[i])) {
                    p.video.width = widths[i];
                    p.video.height = heights[i];
                    p.dirty = true;
                }
            }
            cursor.y += 26.0f;
        }

        ui::drawTextClipped(Rectangle{cursor.x, cursor.y, cursor.width, 14.0f},
                            "Target frame rate", 12.0f, t.textDim);
        cursor.y += 14.0f;
        {
            float fps = static_cast<float>(p.video.fps);
            if (ui::slider(Rectangle{cursor.x, cursor.y, cursor.width, kRow()}, "fps", &fps, 1.0f,
                           240.0f, 1.0f, "%.0f", 8104)) {
                p.video.fps = std::lround(fps);
                p.dirty = true;
            }
        }
        cursor.y += kRow() + 6.0f;

        if (ui::checkbox(Rectangle{cursor.x, cursor.y, cursor.width, kRow()},
                         "Duration follows the audio", &p.video.useAudioDuration)) {
            p.dirty = true;
        }
        cursor.y += kRow() + kGap();
        if (!p.video.useAudioDuration) {
            float duration = static_cast<float>(p.video.duration);
            ui::drawTextClipped(Rectangle{cursor.x, cursor.y, cursor.width, 14.0f},
                                "Duration (s)", 12.0f, t.textDim);
            cursor.y += 14.0f;
            if (ui::slider(Rectangle{cursor.x, cursor.y, cursor.width, kRow()}, nullptr, &duration,
                           0.5f, 900.0f, 0.1f, "%.1f", 8105)) {
                p.video.duration = duration;
                p.dirty = true;
            }
            cursor.y += kRow() + 8.0f;
        } else {
            float trim = static_cast<float>(p.video.trimStart);
            ui::drawTextClipped(Rectangle{cursor.x, cursor.y, cursor.width, 14.0f},
                                "Trim start (s)", 12.0f, t.textDim);
            cursor.y += 14.0f;
            if (ui::slider(Rectangle{cursor.x, cursor.y, cursor.width, kRow()}, nullptr, &trim, 0.0f,
                           static_cast<float>(std::max(1.0, state.clip.duration())), 0.01f,
                           "%.2f", 8106)) {
                p.video.trimStart = trim;
                p.dirty = true;
            }
            cursor.y += kRow() + 8.0f;
        }

        ui::sectionHeader(Rectangle{cursor.x, cursor.y, cursor.width, 18.0f}, "Media");
        cursor.y += 24.0f;
        ui::drawTextClipped(Rectangle{cursor.x, cursor.y, cursor.width, kRow()},
                            state.clip.valid() ? state.clip.path().c_str() : "no audio loaded",
                            11.0f, state.clip.valid() ? t.text : t.danger);
        cursor.y += 20.0f;
        if (state.clip.valid()) {
            const int sourceKbps = static_cast<int>(p.audio.bitRate / 1000);
            std::string source = p.audio.codec.empty() ? "unknown" : audioCodecDisplayName(p.audio.codec);
            if (sourceKbps > 0) source += " " + std::to_string(sourceKbps) + " kbps";
            if (p.audio.transcodedAac) source += " (converted to AAC in memory)";
            char info[256];
            std::snprintf(info, sizeof(info), "%.2f s  %d ch  %d Hz  %s  %d analysis frames",
                          state.clip.duration(), state.clip.channels(), state.clip.sampleRate(),
                          source.c_str(),
                          state.analysis ? static_cast<int>(state.analysis->frames.size()) : 0);
            ui::drawTextClipped(Rectangle{cursor.x, cursor.y, cursor.width, 18.0f}, info, 10.5f,
                                t.textDim);
        }
        cursor.y += 22.0f;
        if (ui::button(Rectangle{cursor.x, cursor.y, cursor.width * 0.5f - 2.0f, 26.0f},
                       "Load audio", true)) {
            openBrowser(state, "audio", "Open audio file",
                        ".wav,.mp3,.flac,.ogg,.m4a,.aac,.opus,.aiff", state.projectDirectory);
        }
        if (ui::button(Rectangle{cursor.x + cursor.width * 0.5f + 2.0f, cursor.y,
                                 cursor.width * 0.5f - 2.0f, 26.0f},
                       "Re-analyse")) {
            if (state.clip.valid()) {
                state.analysis = analyzeAudio(*state.clip.buffer(), AnalysisSettings{}, {},
                                              state.clip.buffer());
                setStatus(state, "Analysis recomputed");
            }
        }
        cursor.y += 34.0f;

        ui::sectionHeader(Rectangle{cursor.x, cursor.y, cursor.width, 18.0f}, "Output");
        cursor.y += 24.0f;
        {
            const std::vector<std::string> containers = supportedContainers();
            int index = 0;
            for (size_t i = 0; i < containers.size(); ++i) {
                if (containers[i] == p.output.container) index = static_cast<int>(i);
            }
            ui::drawTextClipped(Rectangle{cursor.x, cursor.y, 78.0f, kRow()}, "Container", 12.0f,
                                t.textDim);
            const int before = index;
            ui::dropdown(Rectangle{cursor.x + 78.0f, cursor.y, cursor.width - 78.0f, kRow()}, &index,
                         containers, 8107);
            if (index != before) {
                OutputSpec spec = outputSpecForContainer(containers[static_cast<size_t>(index)]);
                spec.crf = p.output.crf;
                p.output = spec;
                refreshOutputAudio(state);
                updateExportExtension(state);
                p.dirty = true;
            }
            cursor.y += kRow() + kGap();
        }
        ui::drawTextClipped(Rectangle{cursor.x, cursor.y, 78.0f, kRow()}, "Video", 12.0f, t.textDim);
        {
            // Encoders this build carries and the container can take; the
            // project's own choice stays visible even when it is unavailable so
            // the setting is never silently rewritten.
            std::vector<std::string> ids;
            std::vector<std::string> labels;
            for (const std::string &id : videoEncodersForContainer(p.output.container)) {
                if (!ffmpeg::hasEncoder(id)) continue;
                ids.push_back(id);
                labels.push_back(videoEncoderLabel(id));
            }
            int index = -1;
            for (size_t i = 0; i < ids.size(); ++i) {
                if (ids[i] == p.output.videoCodec) index = static_cast<int>(i);
            }
            if (index < 0) {
                ids.insert(ids.begin(), p.output.videoCodec);
                labels.insert(labels.begin(),
                              videoEncoderLabel(p.output.videoCodec) + " (unavailable)");
                index = 0;
            }
            const int before = index;
            ui::dropdown(Rectangle{cursor.x + 78.0f, cursor.y, cursor.width - 78.0f, kRow()}, &index,
                         labels, 8112);
            if (index != before && index >= 0 && index < static_cast<int>(ids.size())) {
                p.output.videoCodec = ids[static_cast<size_t>(index)];
                p.dirty = true;
            }
            cursor.y += kRow();
        }
        ui::drawTextClipped(Rectangle{cursor.x, cursor.y, 78.0f, kRow()}, "Audio", 12.0f, t.textDim);
        {
            // Same idea as the video row: only encoders this build has and the
            // container can mux, plus the project's own choice even when it is
            // unavailable here.
            std::vector<std::string> ids;
            std::vector<std::string> labels;
            for (const std::string &id : audioEncodersForContainer(p.output.container)) {
                if (!ffmpeg::hasEncoder(id)) continue;
                ids.push_back(id);
                labels.push_back(audioEncoderLabel(id));
            }
            int index = -1;
            for (size_t i = 0; i < ids.size(); ++i) {
                if (ids[i] == p.output.audioCodec) index = static_cast<int>(i);
            }
            if (index < 0) {
                ids.insert(ids.begin(), p.output.audioCodec);
                labels.insert(labels.begin(),
                              audioEncoderLabel(p.output.audioCodec) + " (unavailable)");
                index = 0;
            }
            const int before = index;
            ui::dropdown(Rectangle{cursor.x + 78.0f, cursor.y, cursor.width - 78.0f, kRow()}, &index,
                         labels, 8113);
            if (index != before && index >= 0 && index < static_cast<int>(ids.size())) {
                p.output.audioCodec = ids[static_cast<size_t>(index)];
                p.dirty = true;
            }
            cursor.y += kRow();
        }
        if (p.audio.transcodedAac && p.output.audioCodec == "aac") {
            ui::drawTextClipped(Rectangle{cursor.x, cursor.y, cursor.width, 14.0f},
                                "imported audio was converted to AAC in memory", 10.5f,
                                palette::withAlpha(t.textDim, 0.9f));
            cursor.y += 16.0f;
        }
        ui::drawTextClipped(Rectangle{cursor.x, cursor.y, cursor.width, 14.0f}, "Quality (CRF)",
                            12.0f, t.textDim);
        cursor.y += 14.0f;
        if (ui::intSlider(Rectangle{cursor.x, cursor.y, cursor.width, kRow()}, nullptr, &p.output.crf,
                          0, 51, 8108)) {
            p.dirty = true;
        }
        cursor.y += kRow() + 8.0f;
        ui::drawTextClipped(Rectangle{cursor.x, cursor.y, 78.0f, kRow()}, "Preset", 12.0f, t.textDim);
        if (ui::textField(Rectangle{cursor.x + 78.0f, cursor.y, cursor.width - 78.0f, kRow()},
                          &p.output.preset, nullptr, 8109)) {
            p.dirty = true;
        }
        cursor.y += kRow() + kGap();
        ui::drawTextClipped(Rectangle{cursor.x, cursor.y, 78.0f, kRow()}, "Audio kbps", 12.0f,
                            t.textDim);
        if (ui::textField(Rectangle{cursor.x + 78.0f, cursor.y, cursor.width - 78.0f, kRow()},
                          &p.output.audioBitrate, nullptr, 8110)) {
            p.dirty = true;
        }
        cursor.y += kRow() + kGap();
        {
            // Output rate: 48 kHz unless the project asks for something else.
            // The decoded media keeps its own rate either way.
            static const std::vector<std::string> rates = {"22050", "32000", "44100", "48000",
                                                           "88200", "96000", "192000"};
            int index = 3;
            for (size_t i = 0; i < rates.size(); ++i) {
                if (std::atoi(rates[i].c_str()) == p.output.audioSampleRate) {
                    index = static_cast<int>(i);
                }
            }
            ui::drawTextClipped(Rectangle{cursor.x, cursor.y, 78.0f, kRow()}, "Rate (Hz)", 12.0f,
                                t.textDim);
            const int before = index;
            ui::dropdown(Rectangle{cursor.x + 78.0f, cursor.y, cursor.width - 78.0f, kRow()}, &index,
                         rates, 8111);
            if (index != before) {
                p.output.audioSampleRate = std::atoi(rates[static_cast<size_t>(index)].c_str());
                p.dirty = true;
            }
            cursor.y += kRow() + 8.0f;
        }

        if (ui::button(Rectangle{cursor.x, cursor.y, cursor.width, 30.0f}, "Export video", true)) {
            state.showExportDialog = true;
            updateExportExtension(state);
        }
        cursor.y += 36.0f;
        if (ui::button(Rectangle{cursor.x, cursor.y, cursor.width, 26.0f}, "Save project as...")) {
            openBrowser(state, "project-save", "Save project", ".pforge", state.projectDirectory);
        }
        cursor.y += 32.0f;
        ui::endScroll();
        finishInspectorScroll(state, view, cursor.y);
        return;
    }

    // ---- selected block ---------------------------------------------------
    ui::drawText(Rectangle{cursor.x, cursor.y, cursor.width, 20.0f}, node->def->label.c_str(),
                 15.0f, t.text, ui::Align::Left, true);
    cursor.y += 22.0f;
    char subtitle[96];
    std::snprintf(subtitle, sizeof(subtitle), "%s  -  block %d", node->def->category.c_str(),
                  node->id);
    ui::drawTextClipped(Rectangle{cursor.x, cursor.y, cursor.width, 16.0f}, subtitle, 11.0f,
                        t.textDim);
    cursor.y += 18.0f;
    ui::drawTextClipped(Rectangle{cursor.x, cursor.y, 48.0f, kRow()}, "Title", 12.0f, t.textDim);
    if (ui::textField(Rectangle{cursor.x + 48.0f, cursor.y, cursor.width - 48.0f, kRow()},
                      &node->title, nullptr, 8111)) {
        state.project.dirty = true;
    }
    cursor.y += kRow() + kGap();
    if (!node->status.empty()) {
        ui::drawTextClipped(Rectangle{cursor.x, cursor.y, cursor.width, 16.0f}, node->status.c_str(),
                            11.0f, t.danger);
        cursor.y += 18.0f;
    }

    drawDescription(cursor, node->def->description);

    {
        std::string inputs;
        for (const PortDesc &port : node->inputPorts()) {
            if (!inputs.empty()) inputs += ", ";
            inputs += port.name;
        }
        if (!inputs.empty()) {
            ui::drawTextClipped(Rectangle{cursor.x, cursor.y, cursor.width, 16.0f},
                                ("in:  " + inputs).c_str(), 10.5f, t.textDim);
            cursor.y += 16.0f;
        }
        std::string outputs;
        for (const PortDesc &port : node->outputPorts()) {
            if (!outputs.empty()) outputs += ", ";
            outputs += port.name;
        }
        if (!outputs.empty()) {
            ui::drawTextClipped(Rectangle{cursor.x, cursor.y, cursor.width, 16.0f},
                                ("out: " + outputs).c_str(), 10.5f, t.textDim);
            cursor.y += 16.0f;
        }
    }

    cursor.y += 6.0f;
    std::string currentGroup;
    for (Param &param : node->params) {
        if (param.group != currentGroup) {
            currentGroup = param.group;
            if (!currentGroup.empty()) {
                ui::sectionHeader(Rectangle{cursor.x, cursor.y, cursor.width, 18.0f},
                                  currentGroup.c_str());
                cursor.y += 24.0f;
            }
        }
        drawParam(state, *node, param, cursor);
    }

    cursor.y += 4.0f;
    if (node->kind == "render.shader") {
        // The block's ports follow the file, so they are rebuilt whenever the
        // path changes (here, or through the shader browser).
        if (ui::button(Rectangle{cursor.x, cursor.y, cursor.width * 0.5f - 2.0f, 26.0f},
                       "Pick .glsl")) {
            openBrowser(state, "shader", "Open fragment shader", ".glsl", state.projectDirectory);
        }
        if (ui::button(Rectangle{cursor.x + cursor.width * 0.5f + 2.0f, cursor.y,
                                 cursor.width * 0.5f - 2.0f, 26.0f},
                       "Reload shaders")) {
            const int reloaded = state.renderer.shaders().reloadChanged();
            char message[64];
            std::snprintf(message, sizeof(message), "Reloaded %d shader(s)", reloaded);
            setStatus(state, message);
            refreshShaderPorts(state, *node, true);
        }
        refreshShaderPorts(state, *node);
        cursor.y += 32.0f;

        // What the file asks for, so the port list is self-explanatory.
        if (!node->pstr("shader").empty()) {
            std::string ports;
            for (const PortDesc &port : node->inputPorts()) {
                if (!ports.empty()) ports += ", ";
                ports += port.name;
            }
            ui::drawTextClipped(Rectangle{cursor.x, cursor.y, cursor.width, 16.0f},
                                ("inputs: " + ports).c_str(), 10.5f, t.textDim);
            cursor.y += 18.0f;
        }
    }
    if (ui::button(Rectangle{cursor.x, cursor.y, cursor.width, 26.0f}, "Delete block")) {
        deleteSelectedNode(state);
        ui::endScroll();
        finishInspectorScroll(state, view, cursor.y);
        return;
    }
    cursor.y += 32.0f;
    ui::endScroll();
    finishInspectorScroll(state, view, cursor.y);
}

}  // namespace pf
