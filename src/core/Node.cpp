#include "core/Node.h"

#include <algorithm>
#include <cmath>

namespace pf {

static const Param kMissingParam{};
static const std::string kEmptyString;

const Param *Node::find(const std::string &key) const {
    for (const auto &param : params) {
        if (param.key == key) return &param;
    }
    return nullptr;
}

Param *Node::find(const std::string &key) {
    for (auto &param : params) {
        if (param.key == key) return &param;
    }
    return nullptr;
}

float Node::pfloat(const std::string &key, float fallback) const {
    const Param *param = find(key);
    return param ? param->value : fallback;
}

int Node::pint(const std::string &key, int fallback) const {
    const Param *param = find(key);
    return param ? param->intValue() : fallback;
}

bool Node::pbool(const std::string &key, bool fallback) const {
    const Param *param = find(key);
    return param ? param->boolean : fallback;
}

Color Node::pcolor(const std::string &key) const {
    const Param *param = find(key);
    return param ? param->color : WHITE;
}

const std::string &Node::pstr(const std::string &key) const {
    const Param *param = find(key);
    return param ? param->text : kEmptyString;
}

float Node::curveAt(const std::string &key, double normalizedTime) const {
    const Param *param = find(key);
    if (!param) return 0.0f;
    return param->evalCurve(normalizedTime);
}

void Node::setFloat(const std::string &key, float value) {
    if (Param *param = find(key)) param->value = value;
}

void Node::setInt(const std::string &key, int value) {
    if (Param *param = find(key)) param->setInt(value);
}

void Node::setBool(const std::string &key, bool value) {
    if (Param *param = find(key)) param->boolean = value;
}

void Node::setText(const std::string &key, const std::string &value) {
    if (Param *param = find(key)) param->text = value;
}

void Node::setColor(const std::string &key, Color value) {
    if (Param *param = find(key)) param->color = value;
}

void Node::ensureParams(const NodeDef &definition) {
    std::vector<Param> merged;
    merged.reserve(definition.params.size());
    for (const Param &defaults : definition.params) {
        Param copy = defaults;
        if (const Param *existing = find(defaults.key)) {
            // Keep the saved value but adopt the current metadata (range, labels,
            // enum options), which keeps older project files working.
            const std::vector<Keyframe> keys = existing->keys;
            const std::string text = existing->text;
            const bool boolean = existing->boolean;
            const Color color = existing->color;
            const float value = existing->value;
            copy.value = value;
            copy.boolean = boolean;
            copy.color = color;
            copy.text = text;
            if (!keys.empty() || defaults.kind == ParamKind::Curve) copy.keys = keys;
        }
        merged.push_back(std::move(copy));
    }
    params = std::move(merged);
}

void Node::pushHistory(std::vector<float> &buffer, float value, int capacity) {
    if (capacity <= 0) return;
    if (static_cast<int>(buffer.size()) != capacity) {
        buffer.assign(static_cast<size_t>(capacity), 0.0f);
    }
    // Shift-in-place is fine for the small buffers used by the block previews
    // (a few dozen samples) and keeps the newest sample at the end.
    std::move(buffer.begin() + 1, buffer.end(), buffer.begin());
    buffer.back() = value;
}

float Node::historyAt(const std::vector<float> &buffer, int capacity, int count, float position) {
    if (buffer.empty() || capacity <= 0) return 0.0f;
    const int filled = std::min(count, capacity);
    if (filled <= 0) return 0.0f;
    const float offsetFromNewest = (1.0f - std::clamp(position, 0.0f, 1.0f)) * (filled - 1);
    const int index = static_cast<int>(std::lround(offsetFromNewest));
    // pushHistory keeps the newest sample at the back, so walk back from there:
    // position 1 is the back, position 0 the oldest sample still held.
    const int clamped =
        std::clamp(static_cast<int>(buffer.size()) - 1 - index, 0,
                   static_cast<int>(buffer.size()) - 1);
    return buffer[static_cast<size_t>(clamped)];
}

void Node::pushWaveHistory(Node &node, int sampleRate, const float *samples, int channels,
                           long long frames, long long startFrame) {
    if (!samples || frames <= 0 || sampleRate <= 0) return;
    const int capacity =
        std::clamp(static_cast<int>(sampleRate * 0.12), 4096, 65536);
    if (static_cast<int>(node.waveHistory.size()) != capacity) {
        node.waveHistory.assign(static_cast<size_t>(capacity), 0.0f);
        node.waveHead = 0;
        node.waveCount = 0;
        node.waveEndFrame = -1;
    }
    // A window that is not the continuation of the ring is a seek or a rewired
    // source: start over instead of mixing two positions of the track.
    const long long oldest = node.waveEndFrame - node.waveCount;
    if (node.waveEndFrame < 0 || startFrame > node.waveEndFrame ||
        startFrame + frames <= oldest) {
        node.waveHead = 0;
        node.waveCount = 0;
        node.waveEndFrame = startFrame;
    }
    const long long begin = std::max(startFrame, node.waveEndFrame);
    const long long end = startFrame + frames;
    const int clampedChannels = std::max(1, channels);
    for (long long frame = begin; frame < end; ++frame) {
        float value = 0.0f;
        const float *source = samples + (frame - startFrame) * clampedChannels;
        for (int channel = 0; channel < clampedChannels; ++channel) value += source[channel];
        value /= static_cast<float>(clampedChannels);
        node.waveHistory[static_cast<size_t>((node.waveHead + node.waveCount) % capacity)] = value;
        if (node.waveCount < capacity) {
            ++node.waveCount;
        } else {
            node.waveHead = (node.waveHead + 1) % capacity;
        }
    }
    // A window that overlaps the ring without adding anything (a repeated
    // display tick, or a small step back) must not move the ring's end.
    if (end > node.waveEndFrame) node.waveEndFrame = end;
}

float Node::waveHistoryAt(double frame) const {
    if (waveHistory.empty() || waveCount <= 0) return 0.0f;
    const double oldest = static_cast<double>(waveEndFrame - waveCount);
    if (frame <= oldest) {
        return waveHistory[static_cast<size_t>(waveHead)];
    }
    if (frame >= static_cast<double>(waveEndFrame)) {
        const int newest = (waveHead + waveCount - 1) % static_cast<int>(waveHistory.size());
        return waveHistory[static_cast<size_t>(newest)];
    }
    const int index = static_cast<int>(frame - oldest);
    const int capacity = static_cast<int>(waveHistory.size());
    const float first = waveHistory[static_cast<size_t>((waveHead + index) % capacity)];
    const float second = waveHistory[static_cast<size_t>((waveHead + index + 1) % capacity)];
    const float t = static_cast<float>(frame - std::floor(frame));
    return first + (second - first) * t;
}

}  // namespace pf
