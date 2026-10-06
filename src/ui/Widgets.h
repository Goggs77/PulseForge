// Immediate-mode widget toolkit drawn with raylib, styled from the active
// CrystalGUI theme.
//
// Widgets that hold interaction state (drag, focus, popups) identify themselves
// with an id handed out by nextId() in draw order; call beginFrame() at the top
// of every frame so the ids stay deterministic.
#pragma once

#include <cmath>
#include <string>
#include <vector>

#include "core/Port.h"
#include "raylib.h"
#include "ui/Theme.h"

namespace pf::ui {

enum class Align { Left = 0, Center = 1, Right = 2 };

// raymath.h cannot be included from C++ (it redefines raylib's vector types),
// so the handful of helpers we need live here.
inline float distance(Vector2 a, Vector2 b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

void beginFrame();
int nextId();

// Stable widget identity derived from the block and parameter it edits, so
// edit and popup state survive layout changes - selecting another block shifts
// the draw order, which would otherwise hand the same id to a different widget.
int widgetId(int nodeId, const char *key);

// Drops every in-progress edit (slider typing, field focus, open list).
void cancelEdits();

// While a modal (dialog or popup) is up, nothing behind it may react to the
// mouse. The app sets this each frame.
void setModal(bool blocked);
bool inputBlocked();

// Corner radius in pixels, converted to raylib's relative roundness so every
// panel and control shares the same visual radius regardless of its size.
float roundness(Rectangle r, float radiusPx);

float textWidth(const char *text, float size, bool bold = false);
void drawText(Rectangle bounds, const char *text, float size, Color color,
              Align align = Align::Left, bool bold = false);
void drawTextClipped(Rectangle bounds, const char *text, float size, Color color,
                     Align align = Align::Left, bool bold = false);
// Word-wrapped paragraphs: the height is measured with the same font atlas the
// text is drawn with, so callers can reserve exactly the space they need
// instead of guessing (which used to clip longer text). `lineHeight` <= 0 picks
// a value from the font size.
float textWrappedHeight(const char *text, float size, float width, float lineHeight = 0.0f,
                        bool bold = false);
float drawTextWrapped(Rectangle bounds, const char *text, float size, Color color,
                      float lineHeight = 0.0f, bool bold = false);

bool hovered(Rectangle r);
// Panels that are busy (async audio loading) block the mouse inside their rect,
// so the widgets there draw normally but cannot be hovered or clicked. Regions
// are set every frame and cleared by beginFrame().
void blockRegion(Rectangle region);
void panel(Rectangle r, const char *title = nullptr);
void sectionHeader(Rectangle r, const char *title);

bool button(Rectangle r, const char *label, bool primary = false, bool enabled = true,
            const char *tooltip = nullptr);
bool toggleButton(Rectangle r, const char *label, bool active, const char *tooltip = nullptr);
bool smallButton(Rectangle r, const char *label, bool active = false);

bool slider(Rectangle r, const char *label, float *value, float lo, float hi, float step = 0.0f,
            const char *format = "%.2f", int stableId = 0, bool logarithmic = false);
// True while that slider is being dragged. The inspector uses it to show the
// base value (instead of the modulation-following readout) during a drag.
bool sliderDragging(int stableId);
bool intSlider(Rectangle r, const char *label, int *value, int lo, int hi, int stableId = 0);
// Commit-on-enter numeric entry with stepper buttons; used for exact values such
// as the video resolution, where a slider is the wrong control.
bool intField(Rectangle r, int *value, int lo, int hi, int stableId = 0);
bool floatField(Rectangle r, float *value, float lo, float hi, const char *format = "%.4g",
                int stableId = 0);
bool checkbox(Rectangle r, const char *label, bool *value);
bool dropdown(Rectangle r, int *value, const std::vector<std::string> &options,
              int stableId = 0);
bool textField(Rectangle r, std::string *value, const char *placeholder = nullptr,
               int stableId = 0);
// Multi-line text area: the buffer keeps real line breaks, long lines wrap
// visually, the caret follows click and arrow keys (including up/down across the
// wrapped lines) and the box scrolls to keep the caret in view. It shares the
// frame's single editor with textField, so only one field edits at a time.
bool textArea(Rectangle r, std::string *value, const char *placeholder = nullptr,
              int stableId = 0);
bool colorField(Rectangle r, Color *value, int stableId = 0);
bool curveEditor(Rectangle r, Param *curve, bool bipolar, double playhead01, int stableId = 0);
void progressBar(Rectangle r, float fraction, const char *label = nullptr);

// Scrolling region: the caller offsets its own y coordinates by *offset.
void beginScroll(Rectangle view);
void endScroll();
void scrollbar(Rectangle track, float *offset, float contentHeight, float viewHeight);
bool scrollWheel(float *offset, float contentHeight, float viewHeight);

// Deferred popups (dropdown lists, colour pickers) are drawn on top of
// everything when drawPopups() is called at the end of the frame.
void drawPopups();
bool popupOpen();

void tooltip(Rectangle anchor, const char *text);
void toast(Rectangle bounds, const char *text, Color color, float alpha);

}  // namespace pf::ui
