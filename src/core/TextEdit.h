// Single-line text editing: caret, selection, clipboard and word movement.
//
// Pure logic with no UI or raylib dependency, so the behaviour that makes an
// input box feel normal (arrows, HOME/END, SHIFT selection, mouse placement)
// can be unit tested without a window.
#pragma once

#include <string>

namespace pf {

// One frame of keyboard input for the active edit.
struct TextEditKeys {
    bool left = false;
    bool right = false;
    bool home = false;
    bool end = false;
    bool backspace = false;
    bool eraseForward = false;
    bool shift = false;
    bool ctrl = false;
    bool selectAll = false;
    bool copy = false;
    bool cut = false;
    bool paste = false;
    bool commit = false;  // Enter
    bool cancel = false;  // Escape
    std::string typed;     // UTF-8 characters typed this frame
    std::string clipboard; // clipboard contents, used by paste
};

// What one frame of input did.
struct TextEditApplied {
    bool changed = false;      // the buffer changed
    bool finished = false;     // the edit should end
    bool commit = false;       // when finished: keep the buffer (Enter) or drop it (Escape)
    bool copied = false;       // the caller should write `clipboard` to the OS clipboard
    std::string clipboard;
};

// Byte offsets, always kept on a UTF-8 character boundary.
int textIndexPrev(const std::string &text, int index);
int textIndexNext(const std::string &text, int index);
int textIndexWordLeft(const std::string &text, int index);
int textIndexWordRight(const std::string &text, int index);
bool textIndexOnBoundary(const std::string &text, int index);

class TextEditState {
public:
    // Starts an edit: the buffer is the current value and the caret lands at the
    // given byte offset (the end by default).
    void begin(const std::string &value, int caretIndex = -1);
    void clear();

    const std::string &text() const { return text_; }
    std::string &text() { return text_; }
    const std::string &original() const { return original_; }
    int caret() const { return caret_; }
    int anchor() const { return anchor_; }
    bool hasSelection() const { return caret_ != anchor_; }
    int selectionMin() const;
    int selectionMax() const;
    std::string selection() const;

    void selectAll();
    void selectWordAt(int index);
    void setCaret(int index, bool extend);
    void insert(const std::string &utf8);
    // Deletes the selection, or one character backwards/forwards.
    void erase(bool forward);
    void eraseWord(bool forward);
    void setTextKeepingCaret(const std::string &value);

private:
    std::string text_;
    std::string original_;
    int caret_ = 0;
    int anchor_ = 0;
};

TextEditApplied applyTextEditKeys(TextEditState &state, const TextEditKeys &keys);

}  // namespace pf
