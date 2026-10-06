#include "ui/Widgets.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "render/Palette.h"
#include "core/TextEdit.h"

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

// Double-click tracking (slider value editing, word selection), keyed by widget id.
std::unordered_map<int, double> gLastClickTime;

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

bool pointIn(Rectangle r, Vector2 p) {
    return p.x >= r.x && p.x < r.x + r.width && p.y >= r.y && p.y < r.y + r.height;
}

// Panels that are busy (async audio loading) block the mouse inside their rect:
// the widgets there keep drawing - normally, without a hover state - but cannot
// be hovered or clicked. The app sets the regions every frame.
std::vector<Rectangle> gBlockedRegions;

bool regionBlocked(Rectangle region) {
    for (const Rectangle &blocked : gBlockedRegions) {
        if (CheckCollisionRecs(region, blocked)) return true;
    }
    return false;
}

bool mouseClickedIn(Rectangle r) {
    return !regionBlocked(r) && pointIn(r, GetMousePosition()) &&
           IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

Color screenTint(Color base, bool isHovered, bool isHeld) {
    if (isHeld) return palette::modulate(base, 1.25f);
    if (isHovered) return palette::modulate(base, 1.12f);
    return base;
}

// ---------------------------------------------------------------------------
// Single-line text editing
// ---------------------------------------------------------------------------
// Every text entry in the app - plain fields, the numeric fields and the
// slider's inline value editor - shares this one editor, so caret movement,
// selection, clipboard handling and horizontal scrolling behave like a normal
// input box everywhere. Indices are byte offsets that always sit on a UTF-8
// character boundary.
struct ActiveEdit {
    pf::TextEditState state;  // caret/selection/buffer, shared with the tests
    int id = -1;              // widget that owns the edit, -1 when idle
    float scroll = 0.0f;      // horizontal scroll in pixels
    float scrollY = 0.0f;     // vertical scroll of a multi-line area
    bool dragging = false;    // mouse drag selection in progress

    bool active() const { return id >= 0; }
};

ActiveEdit gEdit;
// Survives even when the OS clipboard is unavailable.
std::string gEditClipboard;

// Keyboards do not auto-repeat raylib's IsKeyPressed, so held caret keys would
// move one step per press. This adds the usual delay-then-repeat behaviour.
bool keyRepeats(int key) {
    static int heldKey = 0;
    static double nextRepeat = 0.0;
    const double now = GetTime();
    if (IsKeyPressed(key)) {
        heldKey = key;
        nextRepeat = now + 0.42;
        return true;
    }
    if (IsKeyDown(key) && heldKey == key && now >= nextRepeat) {
        nextRepeat = now + 0.05;
        return true;
    }
    if (!IsKeyDown(key) && heldKey == key) heldKey = 0;
    return false;
}

std::string codepointToUtf8(int codepoint) {
    std::string out;
    if (codepoint < 0x80) {
        out.push_back(static_cast<char>(codepoint));
    } else if (codepoint < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
    return out;
}

void textEditBegin(int id, const std::string &value, int caretIndex) {
    gEdit.id = id;
    gEdit.state.begin(value, caretIndex);
    gEdit.scroll = 0.0f;
    gEdit.scrollY = 0.0f;
    gEdit.dragging = false;
    setKeyboardCaptured(true);
}

void textEditEnd() {
    gEdit.id = -1;
    gEdit.state.clear();
    gEdit.dragging = false;
    gEdit.scroll = 0.0f;
    gEdit.scrollY = 0.0f;
    setKeyboardCaptured(false);
}

// Runs the keyboard for the active edit. Returns true when the edit finished
// this frame; *commit tells whether the value should be kept or dropped.
bool textEditUpdate(bool *commit, bool allowNewlines = false) {
    *commit = false;
    if (!gEdit.active()) return false;
    pf::TextEditKeys keys;
    keys.shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    keys.ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    keys.left = keyRepeats(KEY_LEFT);
    keys.right = keyRepeats(KEY_RIGHT);
    keys.home = keyRepeats(KEY_HOME);
    keys.end = keyRepeats(KEY_END);
    keys.backspace = keyRepeats(KEY_BACKSPACE);
    keys.eraseForward = keyRepeats(KEY_DELETE);
    keys.selectAll = keys.ctrl && IsKeyPressed(KEY_A);
    keys.copy = keys.ctrl && IsKeyPressed(KEY_C);
    keys.cut = keys.ctrl && IsKeyPressed(KEY_X);
    keys.paste = keys.ctrl && IsKeyPressed(KEY_V);
    keys.commit = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER);
    keys.cancel = IsKeyPressed(KEY_ESCAPE);
    keys.undo = keys.ctrl && IsKeyPressed(KEY_Z);
    keys.allowNewlines = allowNewlines;
    if (keys.paste) {
        if (const char *clipboard = GetClipboardText()) keys.clipboard = clipboard;
        if (keys.clipboard.empty()) keys.clipboard = gEditClipboard;
    }
    int codepoint = GetCharPressed();
    while (codepoint > 0) {
        if (codepoint >= 32) keys.typed += codepointToUtf8(codepoint);
        codepoint = GetCharPressed();
    }

    const pf::TextEditApplied applied = pf::applyTextEditKeys(gEdit.state, keys);
    if (applied.copied) {
        gEditClipboard = applied.clipboard;
        SetClipboardText(gEditClipboard.c_str());
    }
    *commit = applied.commit;
    return applied.finished;
}

// Left edge of the text inside the field, honouring centring and scrolling.
float textEditOrigin(const Rectangle &field, float size, bool centered, float inset) {
    const float textW = textWidth(gEdit.state.text().c_str(), size);
    const float inner = std::max(6.0f, field.width - inset * 2.0f);
    if (textW <= inner) {
        return centered ? field.x + (field.width - textW) * 0.5f : field.x + inset;
    }
    return field.x + inset - gEdit.scroll;
}

int textEditIndexAt(float x, float origin, float size) {
    const std::string &text = gEdit.state.text();
    int best = 0;
    float bestDistance = 1.0e9f;
    for (int i = 0; i <= static_cast<int>(text.size()); ++i) {
        if (!pf::textIndexOnBoundary(text, i)) continue;
        const float px = origin + textWidth(text.substr(0, static_cast<size_t>(i)).c_str(), size);
        const float distance = std::fabs(px - x);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = i;
        }
    }
    return best;
}

// Starts or continues an edit in this field and puts the caret under the mouse.
void textEditPressAt(int id, const std::string &value, const Rectangle &field, float size,
                     bool centered, bool alreadyEditing) {
    if (!alreadyEditing) textEditBegin(id, value, static_cast<int>(value.size()));
    const float inset = s(6.0f);
    const int index = textEditIndexAt(GetMousePosition().x, textEditOrigin(field, size, centered, inset),
                                      size);
    const bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    const double now = GetTime();
    const auto it = gLastClickTime.find(id);
    const bool doubleClick = it != gLastClickTime.end() && now - it->second < 0.35;
    gLastClickTime[id] = now;
    if (doubleClick) {
        gEdit.state.selectWordAt(index);
    } else {
        gEdit.state.setCaret(index, shift);
    }
    gEdit.dragging = true;
}

void textEditDrag(const Rectangle &field, float size, bool centered) {
    if (!gEdit.dragging) return;
    if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        gEdit.dragging = false;
        return;
    }
    gEdit.state.setCaret(textEditIndexAt(GetMousePosition().x,
                                         textEditOrigin(field, size, centered, s(6.0f)), size),
                         true);
}

// Paints the buffer with selection highlight, caret and horizontal scrolling.
void textEditDraw(const Rectangle &field, float size, Color color, bool centered,
                  const char *placeholder = nullptr, Color placeholderColor = WHITE) {
    const Theme &t = theme();
    const float inset = s(6.0f);
    const Rectangle inner{field.x + inset, field.y, std::max(6.0f, field.width - inset * 2.0f),
                          field.height};
    const std::string &text = gEdit.state.text();
    const float textW = textWidth(text.c_str(), size);
    if (textW <= inner.width) {
        gEdit.scroll = 0.0f;
    } else {
        const float caretOffset =
            textWidth(text.substr(0, static_cast<size_t>(gEdit.state.caret())).c_str(), size);
        if (caretOffset - gEdit.scroll > inner.width) gEdit.scroll = caretOffset - inner.width;
        if (caretOffset - gEdit.scroll < 0.0f) gEdit.scroll = caretOffset;
        gEdit.scroll = std::clamp(gEdit.scroll, 0.0f, textW - inner.width);
    }
    const float origin = textEditOrigin(field, size, centered, inset);
    BeginScissorMode(static_cast<int>(inner.x), static_cast<int>(inner.y),
                     static_cast<int>(inner.width), static_cast<int>(inner.height));
    if (text.empty() && placeholder && *placeholder) {
        drawText(Rectangle{inner.x, inner.y, inner.width, inner.height}, placeholder, size,
                 placeholderColor);
    } else {
        if (gEdit.state.hasSelection()) {
            const float x0 = origin +
                             textWidth(text.substr(0, static_cast<size_t>(
                                                           gEdit.state.selectionMin()))
                                           .c_str(),
                                       size);
            const float x1 = origin +
                             textWidth(text.substr(0, static_cast<size_t>(
                                                           gEdit.state.selectionMax()))
                                           .c_str(),
                                       size);
            DrawRectangle(static_cast<int>(std::floor(x0)),
                          static_cast<int>(field.y + s(3.0f)),
                          static_cast<int>(std::max(1.0f, x1 - x0)),
                          static_cast<int>(field.height - s(6.0f)),
                          palette::withAlpha(t.accent, 0.45f));
        }
        drawText(Rectangle{origin, field.y, std::max(textW, 1.0f), field.height}, text.c_str(),
                 size, color);
        if (std::fmod(static_cast<float>(gCaretBlink), 1.0f) < 0.5f) {
            const float caretX =
                origin + textWidth(
                             text.substr(0, static_cast<size_t>(gEdit.state.caret())).c_str(), size);
            DrawRectangle(static_cast<int>(std::round(caretX)),
                          static_cast<int>(field.y + s(3.0f)), 1,
                          static_cast<int>(field.height - s(6.0f)), t.text);
        }
    }
    EndScissorMode();
}

// Values typed into the numeric fields are applied as they are typed, guarded
// against half-finished input like "-" or "1.", so switching focus cannot lose
// or misplace a value.
bool numericBufferReady(const std::string &buffer) {
    return !buffer.empty() && buffer != "-" && buffer != "+" && buffer != "." && buffer != "-." &&
           buffer != "+.";
}

// Strips a unit suffix ("40 Hz", "1.20x") so the inline editor shows a number.
std::string editableNumber(const std::string &display) {
    std::string value = display;
    while (!value.empty()) {
        const char c = value.back();
        if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E') break;
        value.pop_back();
    }
    return value;
}

}  // namespace

void beginFrame() {
    gNextId = 1;
    gCaretBlink = GetTime();
    gBlockedRegions.clear();
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
    textEditEnd();
    gCurveDragWidget = -1;
    gCurveDragKey = -1;
    if (gPopup.kind != PopupKind::None) {
        gPopup.kind = PopupKind::None;
        gPopup.hasListResult = false;
        gPopup.hasColorResult = false;
    }
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
    const float scaled = textPixels(size);
    return MeasureTextEx(f, text, scaled, scaled / base).x;
}

namespace {

// Greedy word wrap measured with the atlas the text is drawn with. Words longer
// than the line are hard-broken so nothing can overflow sideways.
std::vector<std::string> wrapLines(const std::string &text, float size, float maxWidth,
                                   bool bold) {
    std::vector<std::string> lines;
    if (maxWidth <= 0.0f) {
        lines.push_back(text);
        return lines;
    }
    std::string line;
    std::string word;
    auto fits = [&](const std::string &value) {
        return textWidth(value.c_str(), size, bold) <= maxWidth;
    };
    auto flushWord = [&]() {
        if (word.empty()) return;
        const std::string candidate = line.empty() ? word : line + " " + word;
        if (fits(candidate)) {
            line = candidate;
            word.clear();
            return;
        }
        if (!line.empty()) {
            lines.push_back(line);
            line.clear();
        }
        std::string rest = word;
        while (!fits(rest) && rest.size() > 1) {
            size_t take = rest.size();
            while (take > 1 && !fits(rest.substr(0, take))) --take;
            lines.push_back(rest.substr(0, take));
            rest = rest.substr(take);
        }
        line = rest;
        word.clear();
    };
    for (char c : text) {
        if (c == '\n') {
            flushWord();
            lines.push_back(line);
            line.clear();
        } else if (c == ' ' || c == '\t') {
            flushWord();
        } else {
            word.push_back(c);
        }
    }
    flushWord();
    if (!line.empty() || lines.empty()) lines.push_back(line);
    return lines;
}

float resolvedLineHeight(float size, float lineHeight) {
    if (lineHeight > 0.0f) return lineHeight;
    return textPixels(size) * 1.42f;
}

}  // namespace

float textWrappedHeight(const char *text, float size, float width, float lineHeight, bool bold) {
    if (!text || !*text) return 0.0f;
    const std::vector<std::string> lines = wrapLines(text, size, width, bold);
    return static_cast<float>(lines.size()) * resolvedLineHeight(size, lineHeight);
}

float drawTextWrapped(Rectangle bounds, const char *text, float size, Color color,
                      float lineHeight, bool bold) {
    if (!text || !*text) return 0.0f;
    const float step = resolvedLineHeight(size, lineHeight);
    const std::vector<std::string> lines = wrapLines(text, size, bounds.width, bold);
    float y = bounds.y;
    for (const std::string &line : lines) {
        drawText(Rectangle{bounds.x, y, bounds.width, step}, line.c_str(), size, color,
                 Align::Left, bold);
        y += step;
    }
    return static_cast<float>(lines.size()) * step;
}

void drawText(Rectangle bounds, const char *text, float size, Color color, Align align, bool bold) {
    if (!text || !*text) return;
    const Font &f = theme().font(size, bold);
    const float base = f.baseSize > 0 ? static_cast<float>(f.baseSize) : 10.0f;
    const float scaled = textPixels(size);
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

bool hovered(Rectangle r) { return !regionBlocked(r) && pointIn(r, GetMousePosition()); }

void blockRegion(Rectangle region) {
    if (region.width > 0.0f && region.height > 0.0f) gBlockedRegions.push_back(region);
}

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
            const char *format, int stableId, bool logarithmic) {
    const Theme &t = theme();
    const int id = stableId != 0 ? stableId : nextId();
    const Vector2 mouse = GetMousePosition();
    const bool interactive = !inputBlocked();
    // Frequency controls are mapped logarithmically: on a 20 Hz..20 kHz range a
    // linear track squeezes everything below 1 kHz into the first 5 %.
    const bool logTrack = logarithmic && lo > 0.0f && hi > lo;
    auto toFraction = [&](float v) {
        if (hi <= lo) return 0.0f;
        if (!logTrack) return std::clamp((v - lo) / (hi - lo), 0.0f, 1.0f);
        return std::clamp((std::log(std::clamp(v, lo, hi)) - std::log(lo)) /
                              (std::log(hi) - std::log(lo)),
                          0.0f, 1.0f);
    };
    auto fromFraction = [&](float position) {
        if (!logTrack) return lo + position * (hi - lo);
        return std::exp(std::log(lo) + position * (std::log(hi) - std::log(lo)));
    };
    // When a label is supplied it owns the left part of the row, so the knob can
    // never sit on top of the text.
    const float labelWidth = (label && *label) ? r.width * 0.42f : 0.0f;
    const Rectangle trackArea{r.x + labelWidth, r.y, r.width - labelWidth, r.height};
    const bool isHovered = interactive && pointIn(trackArea, mouse);
    const bool editing = gEdit.active() && gEdit.id == id;
    const float before = *value;

    // Double-clicking anywhere on the row switches to typing the value, whatever
    // its range, so awkward values do not need pixel hunting.
    if (interactive && pointIn(r, mouse) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        const double now = GetTime();
        const auto it = gLastClickTime.find(id);
        if (it != gLastClickTime.end() && now - it->second < 0.35) {
            const std::string shown(TextFormat(format, static_cast<double>(*value)));
            const std::string start = editableNumber(shown);
            textEditBegin(id, start, static_cast<int>(start.size()));
            gLastClickTime.erase(it);
        } else {
            gLastClickTime[id] = now;
        }
    }

    // Clicking anywhere else ends the inline edit, keeping what was typed.
    if (editing && interactive && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !pointIn(r, mouse)) {
        textEditEnd();
    }
    if (editing && gEdit.active() && gEdit.id == id) {
        // Applied as it is typed so switching away mid-edit cannot lose or copy
        // the value into some other block's slider.
        if (numericBufferReady(gEdit.state.text())) {
            *value = std::clamp(static_cast<float>(std::atof(gEdit.state.text().c_str())),
                                std::min(lo, hi), std::max(lo, hi));
        }
        bool commit = false;
        if (textEditUpdate(&commit)) {
            if (!commit) {
                *value = std::clamp(static_cast<float>(std::atof(gEdit.state.original().c_str())),
                                    std::min(lo, hi), std::max(lo, hi));
            }
            textEditEnd();
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
        float next = fromFraction(position);
        if (step > 0.0f) next = std::round(next / step) * step;
        *value = std::clamp(next, std::min(lo, hi), std::max(lo, hi));
    }

    const float fraction = toFraction(*value);
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
    const bool stillEditing = gEdit.active() && gEdit.id == id;
    const std::string valueText =
        stillEditing ? gEdit.state.text()
                     : std::string(TextFormat(format, static_cast<double>(*value)));
    // The value sits on the filled part of the track when it is large and on the
    // empty part when it is small; pick the ink for whichever it lands on, so it
    // stays dark on a light theme and light on a dark theme.
    const float valueTextWidth = textWidth(valueText.c_str(), 11.0f);
    const bool overFill = fraction * track.width + valueTextWidth >= track.width;
    const Color valueInk = overFill ? readableOn(t.accent) : t.text;
    if (stillEditing) {
        DrawRectangleRounded(Rectangle{track.x, track.y, track.width, track.height},
                             roundness(track, track.height * 0.5f), 6, t.panelRaised);
        DrawRectangleRoundedLines(Rectangle{track.x, track.y, track.width, track.height},
                                  roundness(track, track.height * 0.5f), 6, t.accent);
        // The shared editor supplies the caret, selection and scrolling here too.
        textEditDraw(track, 11.0f, t.text, true);
    } else {
        drawTextClipped(Rectangle{track.x + 6.0f, track.y, track.width - 12.0f, trackHeight},
                        valueText.c_str(), 11.0f, valueInk, Align::Right);
    }
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

bool sliderDragging(int stableId) { return stableId != 0 && gActiveSlider == stableId; }

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

    const bool editing = gEdit.active() && gEdit.id == id;
    if (interactive && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        if (pointIn(field, GetMousePosition())) {
            textEditPressAt(id, std::to_string(*value), field, 12.0f, true, editing);
        } else if (editing) {
            const int parsed = std::clamp(std::atoi(gEdit.state.text().c_str()), lo, hi);
            if (parsed != *value) {
                *value = parsed;
                changed = true;
            }
            textEditEnd();
        }
    }
    if (editing && gEdit.active() && gEdit.id == id) {
        textEditDrag(field, 12.0f, true);
        if (numericBufferReady(gEdit.state.text())) {
            const int parsed = std::clamp(std::atoi(gEdit.state.text().c_str()), lo, hi);
            if (parsed != *value) {
                *value = parsed;
                changed = true;
            }
        }
        bool commit = false;
        if (textEditUpdate(&commit)) {
            if (!commit) *value = std::clamp(std::atoi(gEdit.state.original().c_str()), lo, hi);
            textEditEnd();
        }
    }

    DrawRectangleRounded(field, roundness(field, s(4.0f)), 5, editing ? palette::modulate(t.panelAlt, 1.06f) : t.panelAlt);
    DrawRectangleRoundedLines(field, roundness(field, s(4.0f)), 5, editing ? t.accent : t.border);
    if (editing && gEdit.active() && gEdit.id == id) {
        textEditDraw(field, 12.0f, t.text, true);
    } else {
        drawTextClipped(Rectangle{field.x + 6.0f, field.y, field.width - 12.0f, field.height},
                        std::to_string(*value).c_str(), 12.0f, t.text, Align::Center);
    }
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
            // What the list marks as selected until a row is picked: the value
            // the dropdown shows now, not the result of an earlier popup.
            gPopup.listResult = index;
        }
        return false;
    }
    return changed;
}

bool textField(Rectangle r, std::string *value, const char *placeholder, int stableId) {
    const Theme &t = theme();
    const int id = stableId != 0 ? stableId : nextId();
    const bool interactive = !inputBlocked();
    const bool isHovered = interactive && hovered(r);
    const bool editing = gEdit.active() && gEdit.id == id;
    if (interactive && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        if (isHovered) {
            textEditPressAt(id, *value, r, 12.0f, false, editing);
        } else if (editing) {
            *value = gEdit.state.text();
            textEditEnd();
        }
    }
    bool changed = false;
    // The press above may have just ended this edit (clicking outside commits);
    // running the block again would copy the cleared buffer back into the value.
    if (editing && gEdit.active() && gEdit.id == id) {
        textEditDrag(r, 12.0f, false);
        bool commit = false;
        if (textEditUpdate(&commit)) {
            // Escape restores the text the field had when editing started.
            changed = *value != (commit ? gEdit.state.text() : gEdit.state.original());
            *value = commit ? gEdit.state.text() : gEdit.state.original();
            textEditEnd();
        } else {
            changed = *value != gEdit.state.text();
            *value = gEdit.state.text();  // live, so Enter is not needed to keep typing
        }
    }

    DrawRectangleRounded(r, roundness(r, s(5.0f)), 6, editing ? palette::modulate(t.panelAlt, 1.06f) : t.panelAlt);
    DrawRectangleRoundedLines(r, roundness(r, s(5.0f)), 6, editing ? t.accent : t.border);
    if (editing && gEdit.active() && gEdit.id == id) {
        textEditDraw(r, 12.0f, t.text, false, placeholder,
                     palette::withAlpha(t.textDim, 0.85f));
    } else {
        if (value->empty() && placeholder) {
            drawTextClipped(Rectangle{r.x + 8.0f, r.y, r.width - 16.0f, r.height}, placeholder,
                            12.0f, palette::withAlpha(t.textDim, 0.85f));
        } else {
            drawTextClipped(Rectangle{r.x + 8.0f, r.y, r.width - 16.0f, r.height}, value->c_str(),
                            12.0f, t.text);
        }
    }
    return changed;
}

// ---------------------------------------------------------------------------
// Multi-line text area
// ---------------------------------------------------------------------------

namespace {

// Visual lines of a wrapped buffer: byte ranges [begin, end) with the newline
// that ended them excluded. The drawing, the caret and mouse placement all use
// this one layout, so they can never disagree.
struct EditLine {
    int begin = 0;
    int end = 0;
};

std::vector<EditLine> layoutEditLines(const std::string &text, float size, float maxWidth) {
    std::vector<EditLine> lines;
    const int total = static_cast<int>(text.size());
    if (total == 0) {
        lines.push_back(EditLine{0, 0});
        return lines;
    }
    int cursor = 0;
    while (true) {
        EditLine line{cursor, cursor};
        int scan = cursor;
        while (scan < total && text[static_cast<size_t>(scan)] != '\n') {
            const bool firstWord = scan == line.begin;
            int wordEnd = scan;
            while (wordEnd < total && text[static_cast<size_t>(wordEnd)] != ' ' &&
                   text[static_cast<size_t>(wordEnd)] != '\n') {
                ++wordEnd;
            }
            const std::string candidate =
                text.substr(static_cast<size_t>(line.begin),
                            static_cast<size_t>(wordEnd - line.begin));
            if (firstWord || textWidth(candidate.c_str(), size) <= maxWidth) {
                line.end = wordEnd;
                scan = wordEnd;
                while (scan < total && text[static_cast<size_t>(scan)] == ' ') ++scan;
                if (scan > wordEnd) line.end = scan;  // the spaces belong to this line
            } else {
                break;
            }
        }
        if (scan >= total || text[static_cast<size_t>(scan)] == '\n') {
            line.end = scan;
            lines.push_back(line);
            if (scan >= total) break;
            cursor = scan + 1;
            if (cursor >= total) lines.push_back(EditLine{total, total});
            continue;
        }
        // The next word does not fit on this line: wrap before it.
        lines.push_back(line);
        cursor = line.end;
    }
    return lines;
}

int editLineForOffset(const std::vector<EditLine> &lines, int offset) {
    for (size_t i = 0; i < lines.size(); ++i) {
        if (offset <= lines[i].end) return static_cast<int>(i);
    }
    return static_cast<int>(lines.size()) - 1;
}

// Pixel column of a caret inside one line, measured from the line's own left
// edge. Every line starts at the same x, so the caret of a wrapped line must not
// inherit the width of the lines above it.
float editColumnX(const std::string &text, const EditLine &line, int index, float size) {
    const int from = std::clamp(line.begin, 0, static_cast<int>(text.size()));
    const int to = std::clamp(index, from, static_cast<int>(text.size()));
    return textWidth(text.substr(static_cast<size_t>(from), static_cast<size_t>(to - from)).c_str(),
                     size);
}

// Nearest character boundary of one line to a pixel x (relative to the line).
int editIndexAtX(const std::string &text, const EditLine &line, float x, float size) {
    int best = line.begin;
    float bestDistance = 1.0e9f;
    const int end = std::min(line.end, static_cast<int>(text.size()));
    for (int index = line.begin; index <= end; ++index) {
        if (!pf::textIndexOnBoundary(text, index)) continue;
        const float distance = std::fabs(editColumnX(text, line, index, size) - x);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = index;
        }
    }
    return best;
}

}  // namespace

bool textArea(Rectangle r, std::string *value, const char *placeholder, int stableId) {
    const Theme &t = theme();
    const int id = stableId != 0 ? stableId : nextId();
    const bool interactive = !inputBlocked();
    const bool isHovered = interactive && hovered(r);
    const bool editing = gEdit.active() && gEdit.id == id;
    const float size = 12.5f;
    const float lineHeight = textPixels(size) * 1.35f;
    const float inset = s(6.0f);
    const float barWidth = s(7.0f);
    const Rectangle inner{r.x + inset, r.y + inset,
                          std::max(8.0f, r.width - inset * 2.0f - barWidth),
                          std::max(8.0f, r.height - inset * 2.0f)};
    if (interactive && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        if (isHovered) {
            if (!editing) textEditBegin(id, *value, static_cast<int>(value->size()));
            const std::vector<EditLine> lines = layoutEditLines(gEdit.state.text(), size, inner.width);
            const int line = std::clamp(
                static_cast<int>((GetMouseY() - inner.y + gEdit.scrollY) / lineHeight), 0,
                static_cast<int>(lines.size()) - 1);
            const bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
            gEdit.state.setCaret(
                editIndexAtX(gEdit.state.text(), lines[static_cast<size_t>(line)],
                             GetMousePosition().x - inner.x, size),
                shift);
            gEdit.dragging = true;
        } else if (editing) {
            *value = gEdit.state.text();
            textEditEnd();
        }
    }
    bool changed = false;
    if (editing && gEdit.active() && gEdit.id == id) {
        // Enter inserts a line break instead of finishing the edit; Escape ends
        // it and restores the text as usual.
        bool commit = false;
        const bool finished = textEditUpdate(&commit, true);
        if (finished) {
            changed = *value != (commit ? gEdit.state.text() : gEdit.state.original());
            *value = commit ? gEdit.state.text() : gEdit.state.original();
            textEditEnd();
        }
        if (gEdit.active() && gEdit.id == id) {
            // Up/Down walk the *visual* lines: the caret keeps its pixel column.
            const std::vector<EditLine> lines =
                layoutEditLines(gEdit.state.text(), size, inner.width);
            const bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
            const int caret = gEdit.state.caret();
            const int current = editLineForOffset(lines, caret);
            for (int step = 0; step < 2; ++step) {
                const int key = step == 0 ? KEY_UP : KEY_DOWN;
                if (!keyRepeats(key)) continue;
                const int target = std::clamp(current + (step == 0 ? -1 : 1), 0,
                                              static_cast<int>(lines.size()) - 1);
                const float columnX =
                    editColumnX(gEdit.state.text(), lines[static_cast<size_t>(current)], caret,
                                size);
                gEdit.state.setCaret(
                    editIndexAtX(gEdit.state.text(), lines[static_cast<size_t>(target)], columnX,
                                 size),
                    shift);
                break;
            }
            if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) && gEdit.dragging) {
                const std::vector<EditLine> dragLines =
                    layoutEditLines(gEdit.state.text(), size, inner.width);
                const int line = std::clamp(
                    static_cast<int>((GetMouseY() - inner.y + gEdit.scrollY) / lineHeight), 0,
                    static_cast<int>(dragLines.size()) - 1);
                gEdit.state.setCaret(
                    editIndexAtX(gEdit.state.text(), dragLines[static_cast<size_t>(line)],
                                 GetMousePosition().x - inner.x, size),
                    true);
            } else if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                gEdit.dragging = false;
            }
            changed = *value != gEdit.state.text();
            *value = gEdit.state.text();
        }
    }

    DrawRectangleRounded(r, roundness(r, s(5.0f)), 6,
                         editing ? palette::modulate(t.panelAlt, 1.06f) : t.panelAlt);
    DrawRectangleRoundedLines(r, roundness(r, s(5.0f)), 6, editing ? t.accent : t.border);

    const std::string &text = editing ? gEdit.state.text() : *value;
    const std::vector<EditLine> lines = layoutEditLines(text, size, inner.width);
    const float contentHeight = static_cast<float>(lines.size()) * lineHeight;
    // Keep the caret visible, then let a too-long buffer scroll.
    if (editing) {
        const int caretLine = editLineForOffset(lines, gEdit.state.caret());
        const float caretTop = static_cast<float>(caretLine) * lineHeight;
        if (caretTop - gEdit.scrollY < 0.0f) gEdit.scrollY = caretTop;
        if (caretTop + lineHeight - gEdit.scrollY > inner.height) {
            gEdit.scrollY = caretTop + lineHeight - inner.height;
        }
    }
    gEdit.scrollY = std::clamp(gEdit.scrollY, 0.0f, std::max(0.0f, contentHeight - inner.height));

    BeginScissorMode(static_cast<int>(inner.x), static_cast<int>(inner.y),
                     static_cast<int>(inner.width), static_cast<int>(inner.height));
    if (text.empty() && placeholder && *placeholder) {
        drawText(Rectangle{inner.x, inner.y, inner.width, lineHeight}, placeholder, size,
                 palette::withAlpha(t.textDim, 0.85f));
    } else {
        const int selectionMin = editing ? gEdit.state.selectionMin() : 0;
        const int selectionMax = editing ? gEdit.state.selectionMax() : 0;
        for (size_t i = 0; i < lines.size(); ++i) {
            const float lineY = inner.y + static_cast<float>(i) * lineHeight - gEdit.scrollY;
            if (lineY + lineHeight < inner.y) continue;
            if (lineY > inner.y + inner.height) break;
            const EditLine &line = lines[i];
            if (editing && selectionMax > selectionMin && selectionMax > line.begin &&
                selectionMin < line.end) {
                const int from = std::max(selectionMin, line.begin);
                const int to = std::min(selectionMax, line.end);
                const float x0 = inner.x + editColumnX(text, line, from, size);
                const float x1 = inner.x + editColumnX(text, line, to, size);
                DrawRectangle(static_cast<int>(std::floor(x0)), static_cast<int>(lineY),
                              static_cast<int>(std::max(2.0f, x1 - x0)),
                              static_cast<int>(lineHeight),
                              palette::withAlpha(t.accent, 0.45f));
            }
            const std::string visible =
                text.substr(static_cast<size_t>(line.begin),
                            static_cast<size_t>(std::max(0, line.end - line.begin)));
            drawText(Rectangle{inner.x, lineY, std::max(inner.width, 1.0f), lineHeight},
                     visible.c_str(), size, t.text);
        }
        if (editing && std::fmod(static_cast<float>(gCaretBlink), 1.0f) < 0.5f) {
            const int caretLine = editLineForOffset(lines, gEdit.state.caret());
            const float caretX = inner.x + editColumnX(text, lines[static_cast<size_t>(caretLine)],
                                                       gEdit.state.caret(), size);
            const float caretY =
                inner.y + static_cast<float>(caretLine) * lineHeight - gEdit.scrollY;
            DrawRectangle(static_cast<int>(std::floor(caretX)), static_cast<int>(caretY),
                          std::max(1, static_cast<int>(s(1.5f))),
                          static_cast<int>(lineHeight), t.text);
        }
    }
    EndScissorMode();
    if (contentHeight > inner.height) {
        scrollbar(Rectangle{r.x + r.width - barWidth - s(3.0f), inner.y, barWidth,
                            inner.height},
                  &gEdit.scrollY, contentHeight, inner.height);
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
    const bool editing = gEdit.active() && gEdit.id == id;

    if (interactive && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        if (hovered(r)) {
            textEditPressAt(id, std::string(TextFormat(format, static_cast<double>(*value))), r,
                            11.5f, true, editing);
        } else if (editing) {
            if (numericBufferReady(gEdit.state.text())) {
                *value = std::clamp(static_cast<float>(std::atof(gEdit.state.text().c_str())), lo, hi);
            }
            textEditEnd();
        }
    }
    if (editing && gEdit.active() && gEdit.id == id) {
        textEditDrag(r, 11.5f, true);
        if (numericBufferReady(gEdit.state.text())) {
            *value = std::clamp(static_cast<float>(std::atof(gEdit.state.text().c_str())), lo, hi);
        }
        bool commit = false;
        if (textEditUpdate(&commit)) {
            if (!commit) {
                *value = std::clamp(static_cast<float>(std::atof(gEdit.state.original().c_str())), lo, hi);
            }
            textEditEnd();
        }
    }

    const float before = *value;
    DrawRectangleRounded(r, roundness(r, s(4.0f)), 5,
                         editing ? palette::modulate(t.panelAlt, 1.06f) : t.panelAlt);
    DrawRectangleRoundedLines(r, roundness(r, s(4.0f)), 5, editing ? t.accent : t.border);
    if (editing && gEdit.active() && gEdit.id == id) {
        textEditDraw(r, 11.5f, t.text, true);
    } else {
        const std::string shown(TextFormat(format, static_cast<double>(*value)));
        drawTextClipped(Rectangle{r.x + s(5.0f), r.y, r.width - s(10.0f), r.height}, shown.c_str(),
                        11.5f, t.text, Align::Center);
    }
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
            // Stay between the neighbours: dragging a key past one would let
            // the release sort swap them and hand the drag to another key.
            const CurveKeyBounds bounds =
                curveKeyBounds(*curve, static_cast<size_t>(gCurveDragKey));
            key.time = std::clamp(toTime(mouse.x), bounds.lo, bounds.hi);
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
