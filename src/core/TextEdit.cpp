#include "core/TextEdit.h"

#include <algorithm>
#include <cctype>

namespace pf {

namespace {

bool continuationByte(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }

bool wordCharacter(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u >= 0x80) return true;  // part of a multi-byte character
    return std::isalnum(u) != 0 || c == '_' || c == '.' || c == '-' || c == '+' || c == '/' ||
           c == '\\' || c == ':';
}

}  // namespace

int textIndexPrev(const std::string &text, int index) {
    int i = std::clamp(index, 0, static_cast<int>(text.size()));
    if (i == 0) return 0;
    --i;
    while (i > 0 && continuationByte(text[static_cast<size_t>(i)])) --i;
    return i;
}

int textIndexNext(const std::string &text, int index) {
    const int size = static_cast<int>(text.size());
    int i = std::clamp(index, 0, size);
    if (i >= size) return size;
    ++i;
    while (i < size && continuationByte(text[static_cast<size_t>(i)])) ++i;
    return i;
}

bool textIndexOnBoundary(const std::string &text, int index) {
    if (index <= 0 || index >= static_cast<int>(text.size())) return true;
    return !continuationByte(text[static_cast<size_t>(index)]);
}

int textIndexWordLeft(const std::string &text, int index) {
    int i = std::clamp(index, 0, static_cast<int>(text.size()));
    while (i > 0 && !wordCharacter(text[static_cast<size_t>(textIndexPrev(text, i))])) {
        i = textIndexPrev(text, i);
    }
    while (i > 0) {
        const int prev = textIndexPrev(text, i);
        if (!wordCharacter(text[static_cast<size_t>(prev)])) break;
        i = prev;
    }
    return i;
}

int textIndexWordRight(const std::string &text, int index) {
    const int size = static_cast<int>(text.size());
    int i = std::clamp(index, 0, size);
    while (i < size && !wordCharacter(text[static_cast<size_t>(i)])) i = textIndexNext(text, i);
    while (i < size && wordCharacter(text[static_cast<size_t>(i)])) i = textIndexNext(text, i);
    return i;
}

void TextEditState::begin(const std::string &value, int caretIndex) {
    text_ = value;
    original_ = value;
    const int index = caretIndex < 0 ? static_cast<int>(value.size())
                                     : std::clamp(caretIndex, 0, static_cast<int>(value.size()));
    caret_ = anchor_ = index;
}

void TextEditState::clear() {
    text_.clear();
    original_.clear();
    caret_ = anchor_ = 0;
}

int TextEditState::selectionMin() const { return std::min(caret_, anchor_); }
int TextEditState::selectionMax() const { return std::max(caret_, anchor_); }

std::string TextEditState::selection() const {
    const int from = selectionMin();
    return text_.substr(static_cast<size_t>(from),
                        static_cast<size_t>(selectionMax() - from));
}

void TextEditState::selectAll() {
    anchor_ = 0;
    caret_ = static_cast<int>(text_.size());
}

void TextEditState::selectWordAt(int index) {
    int from = std::clamp(index, 0, static_cast<int>(text_.size()));
    int to = from;
    while (from > 0 && wordCharacter(text_[static_cast<size_t>(textIndexPrev(text_, from))])) {
        from = textIndexPrev(text_, from);
    }
    while (to < static_cast<int>(text_.size()) && wordCharacter(text_[static_cast<size_t>(to)])) {
        to = textIndexNext(text_, to);
    }
    anchor_ = from;
    caret_ = to;
}

void TextEditState::setCaret(int index, bool extend) {
    const int clamped = std::clamp(index, 0, static_cast<int>(text_.size()));
    caret_ = textIndexOnBoundary(text_, clamped) ? clamped : textIndexPrev(text_, clamped);
    if (!extend) anchor_ = caret_;
}

void TextEditState::insert(const std::string &utf8) {
    if (utf8.empty()) return;
    const int from = selectionMin();
    const int to = selectionMax();
    text_.replace(static_cast<size_t>(from), static_cast<size_t>(to - from), utf8);
    caret_ = anchor_ = from + static_cast<int>(utf8.size());
}

void TextEditState::erase(bool forward) {
    if (hasSelection()) {
        const int from = selectionMin();
        const int to = selectionMax();
        text_.erase(static_cast<size_t>(from), static_cast<size_t>(to - from));
        caret_ = anchor_ = from;
        return;
    }
    if (forward) {
        if (caret_ >= static_cast<int>(text_.size())) return;
        const int next = textIndexNext(text_, caret_);
        text_.erase(static_cast<size_t>(caret_), static_cast<size_t>(next - caret_));
    } else {
        if (caret_ <= 0) return;
        const int prev = textIndexPrev(text_, caret_);
        text_.erase(static_cast<size_t>(prev), static_cast<size_t>(caret_ - prev));
        caret_ = anchor_ = prev;
    }
}

void TextEditState::eraseWord(bool forward) {
    if (hasSelection()) {
        erase(forward);
        return;
    }
    const int target =
        forward ? textIndexWordRight(text_, caret_) : textIndexWordLeft(text_, caret_);
    if (target == caret_) return;
    const int from = std::min(target, caret_);
    const int to = std::max(target, caret_);
    text_.erase(static_cast<size_t>(from), static_cast<size_t>(to - from));
    caret_ = anchor_ = from;
}

void TextEditState::setTextKeepingCaret(const std::string &value) {
    const int caret = std::min(caret_, static_cast<int>(value.size()));
    const int anchor = std::min(anchor_, static_cast<int>(value.size()));
    text_ = value;
    caret_ = caret;
    anchor_ = anchor;
}

TextEditApplied applyTextEditKeys(TextEditState &state, const TextEditKeys &keys) {
    TextEditApplied result;
    const int beforeCaret = state.caret();
    const int beforeAnchor = state.anchor();
    const std::string before = state.text();

    if (keys.cancel) {
        state.setTextKeepingCaret(state.original());
        state.selectAll();
        state.setCaret(static_cast<int>(state.text().size()), false);
        result.changed = state.text() != before;
        result.finished = true;
        result.commit = false;
        return result;
    }
    if (keys.commit) {
        result.finished = true;
        result.commit = true;
        return result;
    }

    if (keys.selectAll) state.selectAll();
    if (keys.copy || keys.cut) {
        if (state.hasSelection()) {
            result.copied = true;
            result.clipboard = state.selection();
            if (keys.cut) state.erase(true);
        }
    }
    if (keys.paste) {
        std::string clean;
        for (char c : keys.clipboard) {
            if (c != '\n' && c != '\r' && c != '\t') clean.push_back(c);
        }
        state.insert(clean);
    }

    if (keys.left) {
        if (!keys.shift && state.hasSelection()) {
            state.setCaret(state.selectionMin(), false);
        } else {
            const int next = keys.ctrl ? textIndexWordLeft(state.text(), state.caret())
                                       : textIndexPrev(state.text(), state.caret());
            state.setCaret(next, keys.shift);
        }
    }
    if (keys.right) {
        if (!keys.shift && state.hasSelection()) {
            state.setCaret(state.selectionMax(), false);
        } else {
            const int next = keys.ctrl ? textIndexWordRight(state.text(), state.caret())
                                       : textIndexNext(state.text(), state.caret());
            state.setCaret(next, keys.shift);
        }
    }
    if (keys.home) state.setCaret(0, keys.shift);
    if (keys.end) state.setCaret(static_cast<int>(state.text().size()), keys.shift);
    if (keys.backspace) {
        if (keys.ctrl) {
            state.eraseWord(false);
        } else {
            state.erase(false);
        }
    }
    if (keys.eraseForward) {
        if (keys.ctrl) {
            state.eraseWord(true);
        } else {
            state.erase(true);
        }
    }
    if (!keys.typed.empty()) state.insert(keys.typed);

    result.changed = state.text() != before || state.caret() != beforeCaret ||
                     state.anchor() != beforeAnchor;
    return result;
}

}  // namespace pf
