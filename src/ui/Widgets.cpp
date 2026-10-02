#include "ui/Widgets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "render/Palette.h"

namespace pf::ui {

namespace {

int gNextId = 1;
int gActiveSlider = -1;
int gCurveDragWidget = -1;
int gCurveDragKey = -1;
double gCaretBlink = 0.0;
bool gModal = false;
// True while the popup's own contents are being drawn: the popup is the modal,
// so its buttons and fields must stay interactive even though everything behind
// them is blocked.
bool gDrawingPopup = false;

struct PopupDrawScope {
    PopupDrawScope() { gDrawingPopup = true; }
    ~PopupDrawScope() { gDrawingPopup = false; }
};

// Double-click tracking for slider value editing, keyed by widget id.
std::unordered_map<int, double> gLastClickTime;
int gSliderEditId = -1;
std::string gSliderEditBuffer;

enum class PopupKind { None, List, Color };
struct PopupState {
    PopupKind kind = PopupKind::None;
    int ownerId = -1;  // stable id of the widget that owns the popup
    Rectangle anchor{};
    std::vector<std::string> options;
    // Results are handed back to the owner instead of writing through a pointer
    // into another frame's stack.
    int listResult = -1;
    bool hasListResult = false;
    Color colorResult{};
    bool hasColorResult = false;
    Color original{};
    // Hue/saturation/value are kept here rather than re-derived from the colour
    // every frame, so the pivots do not jump when the colour is grey (hue is
    // ambiguous there) or when saturation reaches zero.
    float hue = 0.0f;
    float saturation = 1.0f;
    float valueOf = 1.0f;
    bool openedThisFrame = false;
    float scroll = 0.0f;
};

// Dragging state for the colour popup's wheel and value bar.
bool gColorDraggingWheel = false;
bool gColorDraggingBar = false;

// Hue/saturation wheel, rasterised once and reused for every popup.
Texture2D &colorWheelTexture() {
    static Texture2D texture{};
    if (texture.id == 0) {
        const int size = 192;
        std::vector<unsigned char> pixels(static_cast<size_t>(size) * size * 4, 0);
        const float half = size * 0.5f;
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                const float dx = (x + 0.5f - half) / half;
                const float dy = (y + 0.5f - half) / half;
                const float radius = std::sqrt(dx * dx + dy * dy);
                unsigned char *pixel = &pixels[(static_cast<size_t>(y) * size + x) * 4];
                if (radius > 1.0f) {
                    continue;  // transparent outside the circle
                }
                // Hue runs counter-clockwise from the +x axis so red sits at the
                // right, the usual convention for colour wheels.
                float hue = std::atan2(-dy, dx) / 6.2831853f;
                if (hue < 0.0f) hue += 1.0f;
                const Color color = colorFromHsv(hue, std::min(radius, 1.0f), 1.0f);
                pixel[0] = color.r;
                pixel[1] = color.g;
                pixel[2] = color.b;
                pixel[3] = 255;
            }
        }
        Image image{};
        image.data = pixels.data();
        image.width = size;
        image.height = size;
        image.mipmaps = 1;
        image.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
        texture = LoadTextureFromImage(image);
        if (texture.id != 0) SetTextureFilter(texture, TEXTURE_FILTER_BILINEAR);
    }
    return texture;
}
PopupState gPopup;

int gEditingId = -1;
std::string gEditBuffer;
int gIntEditId = -1;
std::string gIntEditBuffer;

bool pointIn(Rectangle r, Vector2 p) {
    return p.x >= r.x && p.x < r.x + r.width && p.y >= r.y && p.y < r.y + r.height;
}

bool mouseClickedIn(Rectangle r) {
    return pointIn(r, GetMousePosition()) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

Color screenTint(Color base, bool isHovered, bool isHeld) {
    if (isHeld) return palette::modulate(base, 1.25f);
    if (isHovered) return palette::modulate(base, 1.12f);
    return base;
}

// Shared text-entry state machine; `buffer` holds the in-progress string.
void editKeys(std::string &buffer, bool &commit, bool &cancel) {
    int codepoint = GetCharPressed();
    while (codepoint > 0) {
        if (codepoint >= 32 && codepoint < 127) buffer.push_back(static_cast<char>(codepoint));
        codepoint = GetCharPressed();
    }
    if (IsKeyPressed(KEY_BACKSPACE) && !buffer.empty()) buffer.pop_back();
    commit = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER);
    cancel = IsKeyPressed(KEY_ESCAPE);
}

}  // namespace

void beginFrame() {
    gNextId = 1;
    gCaretBlink = GetTime();
}

int nextId() { return gNextId++; }

int widgetId(int nodeId, const char *key) {
    unsigned hash = 2166136261u;
    for (const char *p = key; p && *p; ++p) {
        hash ^= static_cast<unsigned char>(*p);
        hash *= 16777619u;
    }
    hash ^= static_cast<unsigned>(nodeId) * 2654435761u;
    return static_cast<int>(hash & 0x7FFFFFFFu) + 1;
}

void cancelEdits() {
    gActiveSlider = -1;
    gSliderEditId = -1;
    gSliderEditBuffer.clear();
    gEditingId = -1;
    gEditBuffer.clear();
    gIntEditId = -1;
    gIntEditBuffer.clear();
    gCurveDragWidget = -1;
    gCurveDragKey = -1;
    if (gPopup.kind != PopupKind::None) {
        gPopup.kind = PopupKind::None;
        gPopup.hasListResult = false;
        gPopup.hasColorResult = false;
    }
    setKeyboardCaptured(false);
}

void setModal(bool blocked) { gModal = blocked; }

bool inputBlocked() {
    return !gDrawingPopup && (gModal || gPopup.kind != PopupKind::None);
}

float roundness(Rectangle r, float radiusPx) {
    const float minSide = std::min(r.width, r.height);
    if (minSide <= 0.0f) return 0.0f;
    return std::clamp(radiusPx / (minSide * 0.5f), 0.0f, 1.0f);
}

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

float textWidth(const char *text, float size, bool bold) {
    if (!text || !*text) return 0.0f;
    const Font &f = theme().font(size, bold);
    const float base = f.baseSize > 0 ? static_cast<float>(f.baseSize) : 10.0f;
    const float scaled = size * theme().uiScale;
    return MeasureTextEx(f, text, scaled, scaled / base).x;
}

void drawText(Rectangle bounds, const char *text, float size, Color color, Align align, bool bold) {
    if (!text || !*text) return;
    const Font &f = theme().font(size, bold);
    const float base = f.baseSize > 0 ? static_cast<float>(f.baseSize) : 10.0f;
    const float scaled = size * theme().uiScale;
    const float spacing = scaled / base;
    const Vector2 measure = MeasureTextEx(f, text, scaled, spacing);
    Vector2 position{bounds.x, bounds.y + (bounds.height - measure.y) * 0.5f};
    switch (align) {
        case Align::Center: position.x = bounds.x + (bounds.width - measure.x) * 0.5f; break;
        case Align::Right: position.x = bounds.x + bounds.width - measure.x; break;
        default: break;
    }
    DrawTextEx(f, text, Vector2{std::round(position.x), std::round(position.y)}, scaled, spacing,
               color);
}

void drawTextClipped(Rectangle bounds, const char *text, float size, Color color, Align align,
                     bool bold) {
    if (!text || !*text) return;
    std::string value = text;
    if (textWidth(value.c_str(), size, bold) > bounds.width) {
        while (value.size() > 1 && textWidth((value + "...").c_str(), size, bold) > bounds.width) {
            value.pop_back();
        }
        value += "...";
    }
    drawText(bounds, value.c_str(), size, color, align, bold);
}

bool hovered(Rectangle r) { return pointIn(r, GetMousePosition()); }

// ---------------------------------------------------------------------------
// Containers
// ---------------------------------------------------------------------------

void panel(Rectangle r, const char *title) {
    const Theme &t = theme();
    DrawRectangleRounded(r, roundness(r, s(9.0f)), 6, t.panel);
    DrawRectangleRoundedLines(r, roundness(r, s(9.0f)), 6, t.border);
    if (title && *title) {
        // The header shares the panel's corner radius at the top; its bottom
        // corners are squared off so it reads as a title bar.
        const Rectangle header{r.x, r.y, r.width, s(26.0f)};
        DrawRectangleRounded(header, roundness(header, s(9.0f)), 6, t.panelAlt);
        DrawRectangle(static_cast<int>(header.x),
                      static_cast<int>(header.y + header.height * 0.5f),
                      static_cast<int>(header.width),
                      static_cast<int>(header.height * 0.5f + 1.0f), t.panelAlt);
        drawText(Rectangle{r.x + s(10.0f), r.y, r.width - s(20.0f), s(26.0f)}, title, 14.0f, t.text,
                 Align::Left, true);
    }
}

void sectionHeader(Rectangle r, const char *title) {
    drawText(r, title, 12.0f, theme().textDim, Align::Left, true);
    DrawLine(static_cast<int>(r.x), static_cast<int>(r.y + r.height - 2.0f),
             static_cast<int>(r.x + r.width), static_cast<int>(r.y + r.height - 2.0f),
             palette::withAlpha(theme().border, 0.9f));
}

// ---------------------------------------------------------------------------
// Buttons
// ---------------------------------------------------------------------------

bool button(Rectangle r, const char *label, bool primary, bool enabled, const char *tooltipText) {
    const Theme &t = theme();
    const bool interactive = !inputBlocked();
    const bool isHovered = enabled && interactive && hovered(r);
    const bool isHeld = isHovered && IsMouseButtonDown(MOUSE_BUTTON_LEFT);
    Color fill = primary ? t.accent : t.panelRaised;
    if (!enabled) fill = palette::modulate(t.panelAlt, 0.85f);
    fill = screenTint(fill, isHovered, isHeld);
    DrawRectangleRounded(r, roundness(r, s(6.0f)), 6, fill);
    DrawRectangleRoundedLines(r, roundness(r, s(6.0f)), 6, palette::withAlpha(t.border, enabled ? 1.0f : 0.5f));
    const Color textColor = enabled ? (primary ? readableOn(fill) : t.text)
                                    : palette::withAlpha(t.textDim, 0.7f);
    drawText(r, label, 13.0f, textColor, Align::Center, true);
    if (tooltipText && isHovered) tooltip(r, tooltipText);
    return enabled && interactive && mouseClickedIn(r);
}

bool toggleButton(Rectangle r, const char *label, bool active, const char *tooltipText) {
    const Theme &t = theme();
    const bool interactive = !inputBlocked();
    const bool isHovered = interactive && hovered(r);
    Color fill = active ? t.accent : t.panelRaised;
    fill = screenTint(fill, isHovered, isHovered && IsMouseButtonDown(MOUSE_BUTTON_LEFT));
    DrawRectangleRounded(r, roundness(r, s(6.0f)), 6, fill);
    DrawRectangleRoundedLines(r, roundness(r, s(6.0f)), 6, palette::withAlpha(t.border, 1.0f));
    drawText(r, label, 13.0f, active ? readableOn(fill) : t.text, Align::Center, true);
    if (tooltipText && isHovered) tooltip(r, tooltipText);
    return interactive && mouseClickedIn(r);
}

bool smallButton(Rectangle r, const char *label, bool active) {
    const Theme &t = theme();
    const bool interactive = !inputBlocked();
    const bool isHovered = interactive && hovered(r);
    const Color fill = screenTint(active ? t.accent : t.panelAlt, isHovered, false);
    DrawRectangleRounded(r, roundness(r, s(5.0f)), 5, fill);
    DrawRectangleRoundedLines(r, roundness(r, s(5.0f)), 5, palette::withAlpha(t.border, 0.9f));
    drawText(r, label, 11.0f, active ? readableOn(fill) : t.text, Align::Center, true);
    return interactive && mouseClickedIn(r);
}

// ---------------------------------------------------------------------------
// Values
// ---------------------------------------------------------------------------

bool slider(Rectangle r, const char *label, float *value, float lo, float hi, float step,
            const char *format, int stableId) {
    const Theme &t = theme();
    const int id = stableId != 0 ? stableId : nextId();
    const Vector2 mouse = GetMousePosition();
    const bool interactive = !inputBlocked();
    // When a label is supplied it owns the left part of the row, so the knob can
    // never sit on top of the text.
    const float labelWidth = (label && *label) ? r.width * 0.42f : 0.0f;
    const Rectangle trackArea{r.x + labelWidth, r.y, r.width - labelWidth, r.height};
    const bool isHovered = interactive && pointIn(trackArea, mouse);
    const bool editing = gSliderEditId == id;
    const float before = *value;

    // Double-clicking anywhere on the row switches to typing the value, whatever
    // its range, so awkward values do not need pixel hunting.
    if (interactive && pointIn(r, mouse) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        const double now = GetTime();
        const auto it = gLastClickTime.find(id);
        if (it != gLastClickTime.end() && now - it->second < 0.35) {
            gSliderEditId = id;
            gSliderEditBuffer.assign(TextFormat(format, static_cast<double>(*value)));
            setKeyboardCaptured(true);
            gLastClickTime.erase(it);
        } else {
            gLastClickTime[id] = now;
        }
    }

    if (editing) {
        bool commit = false, cancel = false;
        editKeys(gSliderEditBuffer, commit, cancel);
        // Apply as you type so switching away mid-edit cannot lose or copy the
        // value into some other block's slider.
        if (!gSliderEditBuffer.empty() && gSliderEditBuffer != "-" &&
            gSliderEditBuffer != "." && gSliderEditBuffer != "-.") {
            *value = std::clamp(static_cast<float>(std::atof(gSliderEditBuffer.c_str())),
                                std::min(lo, hi), std::max(lo, hi));
        }
        if (commit) {
            gSliderEditId = -1;
            setKeyboardCaptured(false);
        } else if (cancel) {
            gSliderEditId = -1;
            setKeyboardCaptured(false);
        }
    }

    // Only the slider that captured the press follows the mouse; without the id
    // every slider on screen would track the same drag.
    if (isHovered && !editing && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) gActiveSlider = id;
    const bool dragging = !editing && gActiveSlider == id && IsMouseButtonDown(MOUSE_BUTTON_LEFT);
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && gActiveSlider == id) gActiveSlider = -1;

    if (dragging) {
        const float position =
            std::clamp((mouse.x - trackArea.x) / std::max(1.0f, trackArea.width), 0.0f, 1.0f);
        float next = lo + position * (hi - lo);
        if (step > 0.0f) next = std::round(next / step) * step;
        *value = std::clamp(next, std::min(lo, hi), std::max(lo, hi));
    }

    const float fraction = (hi > lo) ? std::clamp((*value - lo) / (hi - lo), 0.0f, 1.0f) : 0.0f;
    const float trackHeight = s(18.0f);
    const Rectangle track{trackArea.x, r.y + (r.height - trackHeight) * 0.5f, trackArea.width,
                          trackHeight};
    DrawRectangleRounded(track, roundness(track, track.height * 0.5f), 6, palette::modulate(t.panelAlt, 0.9f));
    DrawRectangleRounded(Rectangle{track.x, track.y, track.width * fraction, track.height}, roundness(track, track.height * 0.5f), 6,
                         isHovered || dragging ? t.accent : t.accentDim);
    DrawCircleV(Vector2{std::round(track.x + track.width * fraction),
                        std::round(track.y + track.height * 0.5f)},
                s(6.0f), t.text);

    if (label && *label) {
        drawTextClipped(Rectangle{r.x, r.y, labelWidth - 8.0f, r.height}, label, 12.0f, t.text);
    }
    const std::string valueText =
        editing ? gSliderEditBuffer
                : std::string(TextFormat(format, static_cast<double>(*value)));
    // The value sits on the filled part of the track when it is large and on the
    // empty part when it is small; pick the ink for whichever it lands on, so it
    // stays dark on a light theme and light on a dark theme.
    const float valueTextWidth = textWidth(valueText.c_str(), 11.0f);
    const bool overFill = fraction * track.width + valueTextWidth >= track.width;
    const Color valueInk = overFill ? readableOn(t.accent) : t.text;
    if (editing) {
        DrawRectangleRounded(Rectangle{track.x, track.y, track.width, track.height},
                             roundness(track, track.height * 0.5f), 6, t.panelRaised);
        DrawRectangleRoundedLines(Rectangle{track.x, track.y, track.width, track.height},
                                  roundness(track, track.height * 0.5f), 6, t.accent);
    }
    drawTextClipped(Rectangle{track.x + 6.0f, track.y, track.width - 12.0f, trackHeight},
                    valueText.c_str(), 11.0f, editing ? t.text : valueInk, Align::Right);
    return *value != before;
}

bool intSlider(Rectangle r, const char *label, int *value, int lo, int hi, int stableId) {
    float f = static_cast<float>(*value);
    slider(r, label, &f, static_cast<float>(lo), static_cast<float>(hi), 1.0f, "%.0f", stableId);
    const int next = std::clamp(static_cast<int>(std::lround(f)), lo, hi);
    const bool changed = next != *value;
    *value = next;
    return changed;
}

bool intField(Rectangle r, int *value, int lo, int hi, int stableId) {
    const Theme &t = theme();
    const int id = stableId != 0 ? stableId : nextId();
    const bool interactive = !inputBlocked();
    const float stepWidth = s(22.0f);
    const Rectangle minus{r.x, r.y, stepWidth, r.height};
    const Rectangle plus{r.x + r.width - stepWidth, r.y, stepWidth, r.height};
    const Rectangle field{r.x + stepWidth + 3.0f, r.y, r.width - 2.0f * stepWidth - 6.0f, r.height};

    bool changed = false;
    if (interactive && smallButton(minus, "-")) {
        *value = std::max(lo, *value - 1);
        changed = true;
    }
    if (interactive && smallButton(plus, "+")) {
        *value = std::min(hi, *value + 1);
        changed = true;
    }

    const bool editing = gIntEditId == id;
    if (interactive && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        if (pointIn(field, GetMousePosition())) {
            if (!editing) {
                gIntEditId = id;
                gIntEditBuffer = std::to_string(*value);
                setKeyboardCaptured(true);
            }
        } else if (editing) {
            const int parsed = std::clamp(std::atoi(gIntEditBuffer.c_str()), lo, hi);
            if (parsed != *value) {
                *value = parsed;
                changed = true;
            }
            gIntEditId = -1;
            setKeyboardCaptured(false);
        }
    }
    if (editing) {
        bool commit = false, cancel = false;
        editKeys(gIntEditBuffer, commit, cancel);
        if (commit || cancel) {
            if (commit && !gIntEditBuffer.empty()) {
                const int parsed = std::clamp(std::atoi(gIntEditBuffer.c_str()), lo, hi);
                if (parsed != *value) {
                    *value = parsed;
                    changed = true;
                }
            }
            gIntEditId = -1;
            setKeyboardCaptured(false);
        }
    }

    DrawRectangleRounded(field, roundness(field, s(4.0f)), 5, editing ? palette::modulate(t.panelAlt, 1.06f) : t.panelAlt);
    DrawRectangleRoundedLines(field, roundness(field, s(4.0f)), 5, editing ? t.accent : t.border);
    const std::string shown = editing ? gIntEditBuffer : std::to_string(*value);
    drawTextClipped(Rectangle{field.x + 6.0f, field.y, field.width - 12.0f, field.height},
                    shown.c_str(), 12.0f, t.text, Align::Center);
    return changed;
}

bool checkbox(Rectangle r, const char *label, bool *value) {
    const Theme &t = theme();
    const bool interactive = !inputBlocked();
    const float box = std::min(s(18.0f), r.height);
    const Rectangle mark{r.x, r.y + (r.height - box) * 0.5f, box, box};
    const bool isHovered = interactive && hovered(r);
    DrawRectangleRounded(mark, roundness(mark, s(4.0f)), 4, *value ? t.accent : palette::modulate(t.panelAlt, 0.9f));
    DrawRectangleRoundedLines(mark, roundness(mark, s(4.0f)), 4, t.border);
    if (*value) {
        DrawLineEx(Vector2{mark.x + 4.0f, mark.y + box * 0.55f},
                   Vector2{mark.x + box * 0.42f, mark.y + box - 4.0f}, 2.2f, Color{16, 20, 30, 255});
        DrawLineEx(Vector2{mark.x + box * 0.42f, mark.y + box - 4.0f},
                   Vector2{mark.x + box - 3.0f, mark.y + 4.0f}, 2.2f, Color{16, 20, 30, 255});
    }
    if (label) {
        drawTextClipped(Rectangle{r.x + box + 8.0f, r.y, r.width - box - 8.0f, r.height}, label,
                        12.0f, isHovered ? t.text : palette::withAlpha(t.text, 0.92f));
    }
    const bool clicked = interactive && mouseClickedIn(r);
    if (clicked) *value = !*value;
    return clicked;
}

bool dropdown(Rectangle r, int *value, const std::vector<std::string> &options,
              int stableId) {
    const Theme &t = theme();
    const int id = stableId != 0 ? stableId : nextId();
    const bool interactive = !inputBlocked();
    const bool isHovered = interactive && hovered(r);

    // A selection made in a previous frame arrives through the popup state; the
    // widget never hands out a pointer to its own stack.
    bool changed = false;
    if (gPopup.hasListResult && gPopup.ownerId == id) {
        *value = gPopup.listResult;
        gPopup.hasListResult = false;
        changed = true;
    }
    DrawRectangleRounded(r, roundness(r, s(5.0f)), 6, screenTint(t.panelRaised, isHovered, false));
    DrawRectangleRoundedLines(r, roundness(r, s(5.0f)), 6, t.border);
    const int index = std::clamp(*value, 0, static_cast<int>(options.size()) - 1);
    const std::string label = options.empty() ? "-" : options[static_cast<size_t>(index)];
    drawTextClipped(Rectangle{r.x + 8.0f, r.y, r.width - 26.0f, r.height}, label.c_str(), 12.0f,
                    t.text);
    const float cx = r.x + r.width - 14.0f;
    const float cy = r.y + r.height * 0.5f;
    DrawTriangle(Vector2{cx - 4.0f, cy - 2.0f}, Vector2{cx + 4.0f, cy - 2.0f},
                 Vector2{cx, cy + 3.0f}, t.textDim);

    if (interactive && mouseClickedIn(r) && !options.empty()) {
        if (gPopup.kind == PopupKind::List && gPopup.ownerId == id) {
            gPopup.kind = PopupKind::None;
        } else {
            gPopup.kind = PopupKind::List;
            gPopup.ownerId = id;
            gPopup.anchor = r;
            gPopup.options = options;
            gPopup.hasListResult = false;
            gPopup.openedThisFrame = true;
            gPopup.scroll = 0.0f;
        }
        return false;
    }
    return changed;
}

bool textField(Rectangle r, std::string *value, const char *placeholder, int stableId) {
    const Theme &t = theme();
    const int id = stableId != 0 ? stableId : nextId();
    const bool editing = gEditingId == id;
    const bool interactive = !inputBlocked();
    const bool isHovered = interactive && hovered(r);
    if (interactive && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        if (isHovered) {
            if (!editing) {
                gEditingId = id;
                gEditBuffer = *value;
                setKeyboardCaptured(true);
            }
        } else if (editing) {
            *value = gEditBuffer;
            gEditingId = -1;
            setKeyboardCaptured(false);
        }
    }
    if (editing) {
        bool commit = false, cancel = false;
        editKeys(gEditBuffer, commit, cancel);
        if (commit) {
            *value = gEditBuffer;
            gEditingId = -1;
            setKeyboardCaptured(false);
        } else if (cancel) {
            gEditBuffer = *value;
            gEditingId = -1;
            setKeyboardCaptured(false);
        }
    }
    const bool changed = editing && gEditingId != -1 && *value != gEditBuffer;
    if (editing && gEditingId == id) *value = gEditBuffer;

    DrawRectangleRounded(r, roundness(r, s(5.0f)), 6, editing ? palette::modulate(t.panelAlt, 1.06f) : t.panelAlt);
    DrawRectangleRoundedLines(r, roundness(r, s(5.0f)), 6, editing ? t.accent : t.border);
    const std::string &shown = (editing && gEditingId == id) ? gEditBuffer : *value;
    if (shown.empty() && placeholder) {
        drawTextClipped(Rectangle{r.x + 8.0f, r.y, r.width - 16.0f, r.height}, placeholder, 12.0f,
                        palette::withAlpha(t.textDim, 0.85f));
    } else {
        drawTextClipped(Rectangle{r.x + 8.0f, r.y, r.width - 16.0f, r.height}, shown.c_str(), 12.0f,
                        t.text);
    }
    if (editing && gEditingId == id && std::fmod(static_cast<float>(gCaretBlink), 1.0f) < 0.5f) {
        const float caretX = r.x + 8.0f + textWidth(shown.c_str(), 12.0f);
        DrawRectangle(static_cast<int>(caretX), static_cast<int>(r.y + 4.0f), 1,
                      static_cast<int>(r.height - 8.0f), t.text);
    }
    return changed;
}

bool colorField(Rectangle r, Color *value, int stableId) {
    const Theme &t = theme();
    const int id = stableId != 0 ? stableId : nextId();
    const bool interactive = !inputBlocked();

    // Live preview: the popup publishes its working colour and the owning field
    // picks it up on the next frame.
    bool changed = false;
    if (gPopup.hasColorResult && gPopup.ownerId == id) {
        const Color next = gPopup.colorResult;
        if (next.r != value->r || next.g != value->g || next.b != value->b) {
            *value = next;
            changed = true;
        }
        gPopup.hasColorResult = false;
    }
    const float swatch = std::min(r.height, s(20.0f));
    const Rectangle box{r.x, r.y + (r.height - swatch) * 0.5f, swatch, swatch};
    DrawRectangleRounded(box, 0.25f, 5, *value);
    DrawRectangleRoundedLines(box, 0.25f, 5, t.border);
    char hex[16];
    std::snprintf(hex, sizeof(hex), "#%02X%02X%02X", value->r, value->g, value->b);
    drawText(Rectangle{r.x + swatch + 8.0f, r.y, r.width - swatch - 8.0f, r.height}, hex, 12.0f,
             t.textDim);
    if (interactive && mouseClickedIn(r)) {
        if (gPopup.kind == PopupKind::Color && gPopup.ownerId == id) {
            gPopup.kind = PopupKind::None;
        } else {
            gPopup.kind = PopupKind::Color;
            gPopup.ownerId = id;
            gPopup.anchor = r;
            gPopup.colorResult = *value;
            gPopup.original = *value;
            colorToHsv(*value, &gPopup.hue, &gPopup.saturation, &gPopup.valueOf);
            gPopup.hasColorResult = false;
            gPopup.openedThisFrame = true;
            gPopup.scroll = 0.0f;
        }
    }
    return changed;
}

// Plain numeric entry used by the matrix grid, where steppers would be too
// cramped. Click to type, Enter commits, Escape cancels.
bool floatField(Rectangle r, float *value, float lo, float hi, const char *format,
                int stableId) {
    const Theme &t = theme();
    const int id = stableId != 0 ? stableId : nextId();
    const bool interactive = !inputBlocked();
    static int gFloatEditId = -1;
    static std::string gFloatEditBuffer;
    const bool editing = gFloatEditId == id;

    if (interactive && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        if (hovered(r)) {
            if (!editing) {
                gFloatEditId = id;
                gFloatEditBuffer = TextFormat(format, static_cast<double>(*value));
                setKeyboardCaptured(true);
            }
        } else if (editing) {
            *value = std::clamp(static_cast<float>(std::atof(gFloatEditBuffer.c_str())), lo, hi);
            gFloatEditId = -1;
            setKeyboardCaptured(false);
        }
    }
    if (editing) {
        bool commit = false, cancel = false;
        editKeys(gFloatEditBuffer, commit, cancel);
        if (commit || cancel) {
            if (commit) {
                *value = std::clamp(static_cast<float>(std::atof(gFloatEditBuffer.c_str())), lo, hi);
            }
            gFloatEditId = -1;
            setKeyboardCaptured(false);
        }
    }

    const float before = *value;
    DrawRectangleRounded(r, roundness(r, s(4.0f)), 5,
                         editing ? palette::modulate(t.panelAlt, 1.06f) : t.panelAlt);
    DrawRectangleRoundedLines(r, roundness(r, s(4.0f)), 5, editing ? t.accent : t.border);
    const std::string shown =
        editing ? gFloatEditBuffer : std::string(TextFormat(format, static_cast<double>(*value)));
    drawTextClipped(Rectangle{r.x + s(5.0f), r.y, r.width - s(10.0f), r.height}, shown.c_str(),
                    11.5f, t.text, Align::Center);
    return *value != before;
}

bool curveEditor(Rectangle r, Param *curve, bool bipolar, double playhead01, int stableId) {
    if (!curve) return false;
    const Theme &t = theme();
    const int id = stableId != 0 ? stableId : nextId();
    bool changed = false;

    auto toY = [&](float v) { return r.y + (1.0f - v) * r.height; };
    auto toValue = [&](float y) { return std::clamp(1.0f - (y - r.y) / r.height, 0.0f, 1.0f); };
    auto toX = [&](double time) { return r.x + static_cast<float>(time) * r.width; };
    auto toTime = [&](float x) {
        return std::clamp(static_cast<double>((x - r.x) / std::max(1.0f, r.width)), 0.0, 1.0);
    };

    // ---- frame ------------------------------------------------------------
    DrawRectangleRounded(r, roundness(r, s(6.0f)), 4, palette::modulate(t.panelAlt, 0.7f));
    for (int i = 1; i < 4; ++i) {
        const float x = r.x + r.width * static_cast<float>(i) / 4.0f;
        DrawLine(static_cast<int>(x), static_cast<int>(r.y), static_cast<int>(x),
                 static_cast<int>(r.y + r.height), palette::withAlpha(t.border, 0.35f));
    }
    for (int i = 1; i < 4; ++i) {
        const float y = r.y + r.height * static_cast<float>(i) / 4.0f;
        DrawLine(static_cast<int>(r.x), static_cast<int>(y), static_cast<int>(r.x + r.width),
                 static_cast<int>(y), palette::withAlpha(t.border, 0.35f));
    }
    // Zero line: the centre for bipolar curves, the bottom for unipolar ones.
    const float zeroY = bipolar ? toY(0.5f) : toY(0.0f);
    DrawLine(static_cast<int>(r.x), static_cast<int>(zeroY), static_cast<int>(r.x + r.width),
             static_cast<int>(zeroY), palette::withAlpha(t.textDim, 0.7f));
    DrawRectangleRoundedLines(r, roundness(r, s(6.0f)), 4, t.border);

    // Range labels.
    char top[16], bottom[16];
    std::snprintf(top, sizeof(top), "%.0f", bipolar ? 1.0 : 1.0);
    std::snprintf(bottom, sizeof(bottom), "%.0f", bipolar ? -1.0 : 0.0);
    drawText(Rectangle{r.x + 4.0f, r.y, 30.0f, 12.0f}, top, 10.0f,
             palette::withAlpha(t.textDim, 0.9f));
    drawText(Rectangle{r.x + 4.0f, r.y + r.height - 12.0f, 30.0f, 12.0f}, bottom, 10.0f,
             palette::withAlpha(t.textDim, 0.9f));

    // ---- curve ------------------------------------------------------------
    const int samples = std::max(32, static_cast<int>(r.width / 3.0f));
    Vector2 previous{toX(0.0), toY(curve->evalCurve(0.0))};
    for (int i = 1; i <= samples; ++i) {
        const double time = static_cast<double>(i) / samples;
        const Vector2 point{toX(time), toY(curve->evalCurve(time))};
        DrawLineEx(previous, point, 2.0f, t.accentAlt);
        previous = point;
    }

    // Playhead.
    const float playX = toX(std::clamp(playhead01, 0.0, 1.0));
    DrawLine(static_cast<int>(playX), static_cast<int>(r.y), static_cast<int>(playX),
             static_cast<int>(r.y + r.height), palette::withAlpha(t.warn, 0.85f));

    // ---- keys -------------------------------------------------------------
    const Vector2 mouse = GetMousePosition();
    int hoveredKey = -1;
    for (size_t i = 0; i < curve->keys.size(); ++i) {
        const Keyframe &key = curve->keys[i];
        const Vector2 position{toX(key.time), toY(key.value)};
        if (distance(position, mouse) < s(8.0f)) hoveredKey = static_cast<int>(i);
        const bool active = hoveredKey == static_cast<int>(i) || (gCurveDragWidget == id &&
                                                                  gCurveDragKey == static_cast<int>(i));
        DrawCircleV(position, active ? s(6.0f) : s(4.5f), active ? t.warn : t.accent);
    }

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        if (hoveredKey >= 0) {
            gCurveDragWidget = id;
            gCurveDragKey = hoveredKey;
        } else if (pointIn(r, mouse)) {
            Keyframe key;
            key.time = toTime(mouse.x);
            key.value = toValue(mouse.y);
            key.easing = 3;
            curve->keys.push_back(key);
            std::sort(curve->keys.begin(), curve->keys.end(),
                      [](const Keyframe &a, const Keyframe &b) { return a.time < b.time; });
            for (size_t i = 0; i < curve->keys.size(); ++i) {
                if (curve->keys[i].time == key.time) gCurveDragKey = static_cast<int>(i);
            }
            gCurveDragWidget = id;
            changed = true;
        }
    }
    if (gCurveDragWidget == id && gCurveDragKey >= 0 &&
        gCurveDragKey < static_cast<int>(curve->keys.size())) {
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) && pointIn(r, mouse)) {
            Keyframe &key = curve->keys[static_cast<size_t>(gCurveDragKey)];
            key.time = toTime(mouse.x);
            key.value = toValue(mouse.y);
            changed = true;
        }
        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            std::sort(curve->keys.begin(), curve->keys.end(),
                      [](const Keyframe &a, const Keyframe &b) { return a.time < b.time; });
            gCurveDragWidget = -1;
            gCurveDragKey = -1;
        }
    }
    if (hoveredKey >= 0 && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && curve->keys.size() > 2) {
        curve->keys.erase(curve->keys.begin() + hoveredKey);
        changed = true;
    }

    char info[64];
    std::snprintf(info, sizeof(info), "%d keys  -  click to add, drag to move, right-click to delete",
                  static_cast<int>(curve->keys.size()));
    drawText(Rectangle{r.x + 26.0f, r.y + r.height - 13.0f, r.width - 30.0f, 12.0f}, info, 10.0f,
             palette::withAlpha(t.textDim, 0.9f));
    return changed;
}

void progressBar(Rectangle r, float fraction, const char *label) {
    const Theme &t = theme();
    DrawRectangleRounded(r, roundness(r, r.height * 0.5f), 8, palette::modulate(t.panelAlt, 0.9f));
    const float width = std::clamp(fraction, 0.0f, 1.0f) * r.width;
    if (width > 1.0f) {
        DrawRectangleRounded(Rectangle{r.x, r.y, width, r.height}, roundness(r, r.height * 0.5f), 8, t.accent);
    }
    if (label && *label) drawText(r, label, 12.0f, t.text, Align::Center);
}

// ---------------------------------------------------------------------------
// Scrolling
// ---------------------------------------------------------------------------

void beginScroll(Rectangle view) {
    BeginScissorMode(static_cast<int>(view.x), static_cast<int>(view.y),
                     static_cast<int>(view.width), static_cast<int>(view.height));
}
void endScroll() { EndScissorMode(); }

void scrollbar(Rectangle track, float *offset, float contentHeight, float viewHeight) {
    const Theme &t = theme();
    if (contentHeight <= viewHeight + 1.0f) {
        *offset = 0.0f;
        return;
    }
    DrawRectangleRounded(track, roundness(track, track.width * 0.5f), 6, palette::withAlpha(t.panelAlt, 0.6f));
    const float ratio = std::clamp(viewHeight / contentHeight, 0.05f, 1.0f);
    const float thumbHeight = std::max(s(24.0f), track.height * ratio);
    const float maxOffset = contentHeight - viewHeight;
    const float position = maxOffset > 0.0f ? std::clamp(*offset / maxOffset, 0.0f, 1.0f) : 0.0f;
    const Rectangle thumb{track.x, track.y + position * (track.height - thumbHeight), track.width,
                          thumbHeight};
    const bool isHovered = hovered(thumb) || hovered(track);
    DrawRectangleRounded(thumb, roundness(thumb, thumb.width * 0.5f), 6, isHovered ? t.accent : t.accentDim);

    if (pointIn(track, GetMousePosition()) && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        const float local = std::clamp((GetMouseY() - track.y - thumbHeight * 0.5f) /
                                           std::max(1.0f, track.height - thumbHeight),
                                       0.0f, 1.0f);
        *offset = local * maxOffset;
    }
}

bool scrollWheel(float *offset, float contentHeight, float viewHeight) {
    const float wheel = GetMouseWheelMove();
    if (wheel == 0.0f) return false;
    const float maxOffset = std::max(0.0f, contentHeight - viewHeight);
    *offset = std::clamp(*offset - wheel * s(48.0f), 0.0f, maxOffset);
    return true;
}

// ---------------------------------------------------------------------------
// Popups
// ---------------------------------------------------------------------------

bool popupOpen() { return gPopup.kind != PopupKind::None; }

void drawPopups() {
    if (gPopup.kind == PopupKind::None) return;
    PopupDrawScope popupScope;
    const Theme &t = theme();
    const Vector2 mouse = GetMousePosition();
    // The click that opened the popup is still "pressed" during this frame; it
    // must not immediately close it again.
    const bool justOpened = gPopup.openedThisFrame;

    if (gPopup.kind == PopupKind::List) {
        const float rowHeight = s(22.0f);
        const float contentHeight = rowHeight * static_cast<float>(gPopup.options.size());
        // The list never grows past the window: it scrolls instead, so long
        // enumerations stay inside the box.
        const float maxHeight = std::min(s(430.0f), GetScreenHeight() - s(40.0f));
        const float height = std::min(maxHeight, contentHeight + s(8.0f));
        Rectangle box{gPopup.anchor.x, gPopup.anchor.y + gPopup.anchor.height + s(2.0f),
                      std::max(gPopup.anchor.width, s(190.0f)), height};
        if (box.y + box.height > GetScreenHeight() - s(4.0f)) {
            box.y = gPopup.anchor.y - box.height - s(2.0f);
        }
        box.x = std::min(box.x, GetScreenWidth() - box.width - s(6.0f));
        box.y = std::max(s(4.0f), box.y);
        DrawRectangle(static_cast<int>(box.x + 2.0f), static_cast<int>(box.y + 3.0f),
                      static_cast<int>(box.width), static_cast<int>(box.height),
                      palette::withAlpha(BLACK, 0.35f));
        Color popupFill = t.panelRaised;
        popupFill.a = 255;  // translucent layer colours would show the editor through
        DrawRectangleRounded(box, roundness(box, s(8.0f)), 5, popupFill);
        DrawRectangleRoundedLines(box, roundness(box, s(8.0f)), 5, t.accent);

        const Rectangle rows{box.x + s(4.0f), box.y + s(4.0f), box.width - s(10.0f),
                             box.height - s(8.0f)};
        const float maxScroll = std::max(0.0f, contentHeight - rows.height);
        if (pointIn(rows, mouse) && GetMouseWheelMove() != 0.0f) {
            gPopup.scroll = std::clamp(gPopup.scroll - GetMouseWheelMove() * s(40.0f), 0.0f,
                                       maxScroll);
        }
        gPopup.scroll = std::clamp(gPopup.scroll, 0.0f, maxScroll);

        BeginScissorMode(static_cast<int>(rows.x), static_cast<int>(rows.y),
                         static_cast<int>(rows.width), static_cast<int>(rows.height));
        for (size_t i = 0; i < gPopup.options.size(); ++i) {
            const Rectangle row{rows.x, rows.y + rowHeight * static_cast<float>(i) - gPopup.scroll,
                                rows.width, rowHeight};
            if (row.y + row.height < rows.y || row.y > rows.y + rows.height) continue;
            const bool isHovered = pointIn(row, mouse);
            const bool selected = gPopup.listResult == static_cast<int>(i);
            if (isHovered) {
                DrawRectangleRounded(row, roundness(row, s(4.0f)), 4,
                                     palette::withAlpha(t.accent, 0.25f));
            }
            if (selected) {
                DrawRectangle(static_cast<int>(row.x), static_cast<int>(row.y), 3,
                              static_cast<int>(row.height), t.accent);
            }
            drawTextClipped(Rectangle{row.x + 8.0f, row.y, row.width - 12.0f, row.height},
                            gPopup.options[i].c_str(), 13.0f, t.text);
            if (isHovered && !justOpened && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                // Publish the result; the owning dropdown applies it next frame.
                gPopup.listResult = static_cast<int>(i);
                gPopup.hasListResult = true;
                gPopup.kind = PopupKind::None;
                gPopup.openedThisFrame = false;
                return;
            }
        }
        EndScissorMode();
        if (maxScroll > 0.0f) {
            scrollbar(Rectangle{box.x + box.width - s(9.0f), rows.y, s(6.0f), rows.height},
                      &gPopup.scroll, contentHeight, rows.height);
        }
        if (!justOpened && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !pointIn(box, mouse)) {
            gPopup.kind = PopupKind::None;
        }
        if (!justOpened && IsKeyPressed(KEY_ESCAPE)) gPopup.kind = PopupKind::None;
        gPopup.openedThisFrame = false;
        return;
    }

    if (gPopup.kind == PopupKind::Color) {
        // --- geometry ------------------------------------------------------
        const float padding = s(12.0f);
        const float wheelSize = s(176.0f);
        const float barWidth = s(26.0f);
        const float rowHeight = s(24.0f);
        const float width = padding * 2.0f + wheelSize + s(10.0f) + barWidth;
        const float height = padding * 2.0f + s(20.0f) /*title*/ + wheelSize + s(10.0f) +
                             s(22.0f) /*swatch*/ + s(8.0f) + rowHeight * 3.0f + s(10.0f) +
                             s(28.0f) /*buttons*/;
        Rectangle box{gPopup.anchor.x, gPopup.anchor.y + gPopup.anchor.height + s(2.0f), width,
                      height};
        if (box.y + box.height > GetScreenHeight() - s(4.0f)) {
            box.y = gPopup.anchor.y - box.height - s(2.0f);
        }
        box.x = std::clamp(box.x, s(4.0f), GetScreenWidth() - box.width - s(6.0f));
        box.y = std::max(s(4.0f), box.y);
        DrawRectangle(static_cast<int>(box.x + s(2.0f)), static_cast<int>(box.y + s(3.0f)),
                      static_cast<int>(box.width), static_cast<int>(box.height),
                      palette::withAlpha(BLACK, 0.35f));
        Color popupFill = t.panelRaised;
        popupFill.a = 255;  // translucent layer colours would show the editor through
        DrawRectangleRounded(box, roundness(box, s(8.0f)), 5, popupFill);
        DrawRectangleRoundedLines(box, roundness(box, s(8.0f)), 5, t.accent);

        const Rectangle wheel{box.x + padding, box.y + padding + s(20.0f), wheelSize, wheelSize};
        const Rectangle bar{wheel.x + wheel.width + s(10.0f), wheel.y, barWidth, wheelSize};

        // --- hue / saturation wheel ---------------------------------------
        Texture2D &wheelTexture = colorWheelTexture();
        if (wheelTexture.id != 0) {
            DrawTexturePro(wheelTexture,
                           Rectangle{0, 0, static_cast<float>(wheelTexture.width),
                                     static_cast<float>(wheelTexture.height)},
                           wheel, Vector2{0, 0}, 0.0f, WHITE);
        }
        DrawCircleLinesV(Vector2{wheel.x + wheel.width * 0.5f, wheel.y + wheel.height * 0.5f},
                         wheel.width * 0.5f, palette::withAlpha(t.border, 0.9f));

        const Vector2 wheelCenter{wheel.x + wheel.width * 0.5f, wheel.y + wheel.height * 0.5f};
        const float wheelRadius = wheel.width * 0.5f;
        auto wheelPivot = [&]() {
            const float angle = gPopup.hue * 6.2831853f;
            return Vector2{wheelCenter.x + std::cos(angle) * gPopup.saturation * wheelRadius,
                           wheelCenter.y - std::sin(angle) * gPopup.saturation * wheelRadius};
        };

        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            if (ui::distance(wheelPivot(), mouse) < s(10.0f) ||
                ui::distance(wheelCenter, mouse) <= wheelRadius) {
                gColorDraggingWheel = true;
                gColorDraggingBar = false;
            } else if (pointIn(bar, mouse)) {
                gColorDraggingBar = true;
                gColorDraggingWheel = false;
            }
        }
        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            gColorDraggingWheel = false;
            gColorDraggingBar = false;
        }
        if (gColorDraggingWheel) {
            const float dx = mouse.x - wheelCenter.x;
            const float dy = mouse.y - wheelCenter.y;
            float hue = std::atan2(-dy, dx) / 6.2831853f;
            if (hue < 0.0f) hue += 1.0f;
            gPopup.hue = hue;
            gPopup.saturation = std::clamp(std::sqrt(dx * dx + dy * dy) / wheelRadius, 0.0f, 1.0f);
        }

        // --- value bar ------------------------------------------------------
        // Black at the bottom to the chosen hue/saturation at the top, so the
        // bar reads as "how bright is this colour".
        const Color pure = colorFromHsv(gPopup.hue, gPopup.saturation, 1.0f);
        DrawRectangleGradientV(static_cast<int>(bar.x), static_cast<int>(bar.y),
                               static_cast<int>(bar.width), static_cast<int>(bar.height), pure,
                               BLACK);
        DrawRectangleRoundedLines(bar, roundness(bar, s(6.0f)), 4,
                                  palette::withAlpha(t.border, 0.9f));
        const float barPivotY = bar.y + (1.0f - gPopup.valueOf) * bar.height;
        if (gColorDraggingBar) {
            gPopup.valueOf = std::clamp(1.0f - (mouse.y - bar.y) / bar.height, 0.0f, 1.0f);
        }
        DrawRectangleRounded(Rectangle{bar.x - s(2.0f), barPivotY - s(3.0f), bar.width + s(4.0f),
                                       s(6.0f)},
                             roundness(bar, s(3.0f)), 4, t.text);
        DrawRectangleRoundedLines(Rectangle{bar.x - s(2.0f), barPivotY - s(3.0f),
                                            bar.width + s(4.0f), s(6.0f)},
                                  roundness(bar, s(3.0f)), 4, palette::withAlpha(BLACK, 0.7f));

        // --- pivots ---------------------------------------------------------
        const Vector2 pivot = wheelPivot();
        DrawCircleV(pivot, s(7.0f), palette::withAlpha(BLACK, 0.55f));
        DrawCircleV(pivot, s(5.5f), colorFromHsv(gPopup.hue, gPopup.saturation, gPopup.valueOf));
        DrawCircleLinesV(pivot, s(5.5f), WHITE);

        // --- colour maths ---------------------------------------------------
        Color &color = gPopup.colorResult;
        color = colorFromHsv(gPopup.hue, gPopup.saturation, gPopup.valueOf);

        // --- preview swatch -------------------------------------------------
        const Rectangle swatch{wheel.x, wheel.y + wheel.height + s(10.0f),
                               wheel.width + s(10.0f) + barWidth, s(22.0f)};
        DrawRectangleRounded(swatch, roundness(swatch, s(4.0f)), 5, color);
        DrawRectangleRoundedLines(swatch, roundness(swatch, s(4.0f)), 5,
                                  palette::withAlpha(t.border, 0.9f));

        // --- numeric entry --------------------------------------------------
        // Six fields edited independently; whichever group the user touched wins
        // for that frame, and the other group is rewritten from it.
        const float columnWidth = (swatch.width - s(8.0f)) * 0.5f;
        const float fieldWidth = columnWidth - s(24.0f);
        const Rectangle rgbColumn{swatch.x, swatch.y + swatch.height + s(8.0f), columnWidth,
                                  rowHeight * 3.0f};
        const Rectangle hsvColumn{rgbColumn.x + columnWidth + s(8.0f), rgbColumn.y, columnWidth,
                                  rowHeight * 3.0f};
        int rgbValues[3] = {color.r, color.g, color.b};
        int hsvValues[3] = {static_cast<int>(std::lround(gPopup.hue * 360.0f)),
                            static_cast<int>(std::lround(gPopup.saturation * 100.0f)),
                            static_cast<int>(std::lround(gPopup.valueOf * 100.0f))};
        const char *rgbNames[3] = {"R", "G", "B"};
        const char *hsvNames[3] = {"H", "S", "V"};
        bool rgbChanged = false;
        bool hsvChanged = false;
        // Plain numeric boxes rather than stepper fields: the columns are too
        // narrow for the -/+ buttons and the digits were being truncated.
        auto intBox = [](Rectangle field, int *value, int lo, int hi, int id) {
            float typed = static_cast<float>(*value);
            if (!floatField(field, &typed, static_cast<float>(lo), static_cast<float>(hi), "%.0f",
                            id)) {
                return false;
            }
            const int rounded = std::clamp(static_cast<int>(std::lround(typed)), lo, hi);
            if (rounded == *value) return false;
            *value = rounded;
            return true;
        };
        for (int i = 0; i < 3; ++i) {
            const float y = rgbColumn.y + rowHeight * static_cast<float>(i);
            drawText(Rectangle{rgbColumn.x, y, s(20.0f), rowHeight}, rgbNames[i], 11.5f, t.textDim);
            if (intBox(Rectangle{rgbColumn.x + s(20.0f), y + s(1.0f), fieldWidth, rowHeight - s(2.0f)},
                       &rgbValues[i], 0, 255, 72001 + i)) {
                rgbChanged = true;
            }
            drawText(Rectangle{hsvColumn.x, y, s(20.0f), rowHeight}, hsvNames[i], 11.5f, t.textDim);
            if (intBox(Rectangle{hsvColumn.x + s(20.0f), y + s(1.0f), fieldWidth, rowHeight - s(2.0f)},
                       &hsvValues[i], 0, i == 0 ? 360 : 100, 72004 + i)) {
                hsvChanged = true;
            }
        }
        if (rgbChanged) {
            const Color edited{static_cast<unsigned char>(rgbValues[0]),
                               static_cast<unsigned char>(rgbValues[1]),
                               static_cast<unsigned char>(rgbValues[2]), color.a};
            colorToHsv(edited, &gPopup.hue, &gPopup.saturation, &gPopup.valueOf);
        } else if (hsvChanged) {
            gPopup.hue = static_cast<float>(hsvValues[0]) / 360.0f;
            gPopup.saturation = static_cast<float>(hsvValues[1]) / 100.0f;
            gPopup.valueOf = static_cast<float>(hsvValues[2]) / 100.0f;
        }
        color = colorFromHsv(gPopup.hue, gPopup.saturation, gPopup.valueOf);
        // Publish every frame so the owning field previews the colour live.
        gPopup.hasColorResult = true;

        // --- buttons --------------------------------------------------------
        const Rectangle selectBox{box.x + box.width - padding - s(150.0f),
                                  box.y + box.height - padding - s(26.0f), s(84.0f), s(26.0f)};
        const Rectangle cancelBox{box.x + box.width - padding - s(60.0f),
                                  box.y + box.height - padding - s(26.0f), s(60.0f), s(26.0f)};
        if (button(selectBox, "Select", true)) {
            gPopup.kind = PopupKind::None;
        }
        if (button(cancelBox, "Cancel")) {
            gPopup.colorResult = gPopup.original;
            gPopup.hasColorResult = true;
            gPopup.kind = PopupKind::None;
        }
        // Escape cancels, clicking anywhere else keeps the colour: that is
        // exactly what the Select button does.
        if (!justOpened && IsKeyPressed(KEY_ESCAPE)) {
            gPopup.colorResult = gPopup.original;
            gPopup.hasColorResult = true;
            gPopup.kind = PopupKind::None;
        }
        if (!justOpened && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !pointIn(box, mouse)) {
            gPopup.kind = PopupKind::None;
        }
        gPopup.openedThisFrame = false;
    }
}

void tooltip(Rectangle anchor, const char *text) {
    if (!text || !*text) return;
    const Theme &t = theme();
    const float width = textWidth(text, 12.0f) + 16.0f;
    Rectangle box{anchor.x, anchor.y + anchor.height + 6.0f, width, 20.0f};
    box.x = std::min(box.x, GetScreenWidth() - box.width - 4.0f);
    box.y = std::min(box.y, GetScreenHeight() - box.height - 4.0f);
    DrawRectangleRounded(box, roundness(box, s(6.0f)), 5, palette::withAlpha(t.panelRaised, 0.96f));
    DrawRectangleRoundedLines(box, roundness(box, s(6.0f)), 5, t.border);
    drawText(box, text, 12.0f, t.text, Align::Center);
}

void toast(Rectangle bounds, const char *text, Color color, float alpha) {
    if (!text || !*text || alpha <= 0.0f) return;
    const Theme &t = theme();
    const float width = std::min(bounds.width - s(20.0f), textWidth(text, 13.0f) + s(28.0f));
    const Rectangle box{bounds.x + (bounds.width - width) * 0.5f, bounds.y + 12.0f, width, 30.0f};
    DrawRectangleRounded(box, roundness(box, s(6.0f)), 6,
                         palette::withAlpha(palette::modulate(t.panelRaised, 0.9f), alpha * 0.92f));
    DrawRectangleRoundedLines(box, roundness(box, s(6.0f)), 6, palette::withAlpha(color, alpha));
    drawText(box, text, 13.0f, palette::withAlpha(color, alpha), Align::Center);
}

}  // namespace pf::ui
