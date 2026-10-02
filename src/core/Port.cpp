#include "core/Port.h"

#include <algorithm>

namespace pf {

const char *portTypeName(PortType type) {
    switch (type) {
        case PortType::Audio: return "Audio";
        case PortType::Analysis: return "Analysis";
        case PortType::Scalar: return "Scalar";
        case PortType::Color: return "Color";
        case PortType::Image: return "Image";
        case PortType::Text: return "Text";
        case PortType::Vec2: return "Vector2";
        case PortType::Vec3: return "Vector3";
        case PortType::Vec4: return "Vector4";
        case PortType::Matrix: return "Matrix";
        default: return "None";
    }
}

Color portTypeColor(PortType type) {
    switch (type) {
        case PortType::Audio: return Color{240, 176, 64, 255};
        case PortType::Analysis: return Color{128, 216, 128, 255};
        case PortType::Scalar: return Color{120, 190, 255, 255};
        case PortType::Color: return Color{236, 120, 200, 255};
        case PortType::Image: return Color{190, 140, 255, 255};
        case PortType::Text: return Color{220, 220, 220, 255};
        case PortType::Vec2: return Color{130, 226, 214, 255};
        case PortType::Vec3: return Color{150, 200, 246, 255};
        case PortType::Vec4: return Color{186, 178, 250, 255};
        case PortType::Matrix: return Color{246, 206, 140, 255};
        default: return GRAY;
    }
}

Value convertValue(const Value &source, PortType target) {
    if (source.type == target) return source;
    if (target == PortType::Vec3 && source.type == PortType::Vec2) {
        return Value::makeVec3(Vector3{source.vec2.x, source.vec2.y, 0.0f});
    }
    if (target == PortType::Vec4 && source.type == PortType::Vec2) {
        return Value::makeVec4(Vector4{source.vec2.x, source.vec2.y, 0.0f, 0.0f});
    }
    if (target == PortType::Vec4 && source.type == PortType::Vec3) {
        return Value::makeVec4(Vector4{source.vec3.x, source.vec3.y, source.vec3.z, 0.0f});
    }
    return Value{};
}

// ---------------------------------------------------------------------------
// AudioBuffer
// ---------------------------------------------------------------------------

float AudioBuffer::monoAt(double frame) const {
    if (frameCount <= 0 || channels <= 0 || samples.empty()) return 0.0f;
    if (frame <= 0.0) {
        return channels == 1 ? samples[0]
                             : (samples[0] + samples[std::min(1, channels - 1)]) * 0.5f;
    }
    const double maxFrame = static_cast<double>(frameCount - 1);
    if (frame >= maxFrame) {
        const size_t base = static_cast<size_t>(frameCount - 1) * channels;
        float sum = 0.0f;
        for (int c = 0; c < channels; ++c) sum += samples[base + static_cast<size_t>(c)];
        return sum / channels;
    }
    const size_t i0 = static_cast<size_t>(frame);
    const size_t i1 = i0 + 1;
    const float t = static_cast<float>(frame - static_cast<double>(i0));
    float a = 0.0f, b = 0.0f;
    for (int c = 0; c < channels; ++c) {
        a += samples[i0 * static_cast<size_t>(channels) + static_cast<size_t>(c)];
        b += samples[i1 * static_cast<size_t>(channels) + static_cast<size_t>(c)];
    }
    a /= channels;
    b /= channels;
    return a + (b - a) * t;
}

// ---------------------------------------------------------------------------
// AnalysisData
// ---------------------------------------------------------------------------

size_t AnalysisData::frameIndexAt(double time) const {
    if (frames.empty()) return 0;
    if (hopSize <= 0 || sampleRate <= 0) return 0;
    const double index = time * sampleRate / hopSize;
    if (index <= 0.0) return 0;
    const size_t last = frames.size() - 1;
    if (index >= static_cast<double>(last)) return last;
    return static_cast<size_t>(index);
}

const AnalysisFrame *AnalysisData::frameAt(double time) const {
    if (frames.empty()) return nullptr;
    return &frames[frameIndexAt(time)];
}

float AnalysisData::bandAt(double time, int band) const {
    const AnalysisFrame *frame = frameAt(time);
    if (!frame || frame->bands.empty()) return 0.0f;
    const int index = std::clamp(band, 0, static_cast<int>(frame->bands.size()) - 1);
    return frame->bands[static_cast<size_t>(index)];
}

float AnalysisData::valueAt(double time, int kind) const {
    const AnalysisFrame *frame = frameAt(time);
    if (!frame) return 0.0f;
    switch (kind) {
        case 0: return frame->rms;
        case 1: return frame->level;
        case 2: return frame->flux;
        case 3: return frame->onset;
        default: return 0.0f;
    }
}

const float *AnalysisData::spectrumRow(double time) const {
    if (spectrum.empty() || spectrumBins <= 0) return nullptr;
    const size_t frameIndex = frameIndexAt(time);
    const size_t offset = frameIndex * static_cast<size_t>(spectrumBins);
    if (offset + static_cast<size_t>(spectrumBins) > spectrum.size()) {
        return spectrum.data() + (spectrum.size() - static_cast<size_t>(spectrumBins));
    }
    return spectrum.data() + offset;
}

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------

Param makeParam(const std::string &key, const std::string &label, float value, float minValue,
                float maxValue, float step, const std::string &group, bool logarithmic) {
    Param p;
    p.key = key;
    p.label = label;
    p.kind = ParamKind::Float;
    p.value = value;
    p.minValue = minValue;
    p.maxValue = maxValue;
    p.step = step;
    p.group = group;
    p.logarithmic = logarithmic;
    return p;
}

Param makeIntParam(const std::string &key, const std::string &label, int value, int minValue,
                   int maxValue, const std::string &group) {
    Param p;
    p.key = key;
    p.label = label;
    p.kind = ParamKind::Int;
    p.value = static_cast<float>(value);
    p.minValue = static_cast<float>(minValue);
    p.maxValue = static_cast<float>(maxValue);
    p.step = 1.0f;
    p.group = group;
    return p;
}

Param makeBoolParam(const std::string &key, const std::string &label, bool value,
                    const std::string &group) {
    Param p;
    p.key = key;
    p.label = label;
    p.kind = ParamKind::Bool;
    p.boolean = value;
    p.group = group;
    return p;
}

Param makeColorParam(const std::string &key, const std::string &label, Color value,
                     const std::string &group) {
    Param p;
    p.key = key;
    p.label = label;
    p.kind = ParamKind::Color;
    p.color = value;
    p.group = group;
    return p;
}

Param makeEnumParam(const std::string &key, const std::string &label,
                    const std::vector<std::string> &options, int index, const std::string &group) {
    Param p;
    p.key = key;
    p.label = label;
    p.kind = ParamKind::Enum;
    p.options = options;
    p.value = static_cast<float>(std::clamp(index, 0, static_cast<int>(options.size()) - 1));
    p.minValue = 0.0f;
    p.maxValue = static_cast<float>(options.empty() ? 0 : options.size() - 1);
    p.step = 1.0f;
    p.group = group;
    return p;
}

Param makeTextParam(const std::string &key, const std::string &label, const std::string &value,
                    const std::string &group) {
    Param p;
    p.key = key;
    p.label = label;
    p.kind = ParamKind::Text;
    p.text = value;
    p.group = group;
    return p;
}

Param makeFileParam(const std::string &key, const std::string &label, const std::string &value,
                    const std::string &extension, const std::string &group) {
    Param p;
    p.key = key;
    p.label = label;
    p.kind = ParamKind::File;
    p.text = value;
    p.hint = extension;
    p.group = group;
    return p;
}

Param makeCurveParam(const std::string &key, const std::string &label) {
    Param p;
    p.key = key;
    p.label = label;
    p.kind = ParamKind::Curve;
    p.minValue = 0.0f;
    p.maxValue = 1.0f;
    p.keys.push_back(Keyframe{0.0, 0.0f, 0});
    p.keys.push_back(Keyframe{1.0, 1.0f, 0});
    return p;
}

Param makeMatrixParam(const std::string &key, const std::string &label, int size) {
    Param p;
    p.key = key;
    p.label = label;
    p.kind = ParamKind::Matrix;
    p.matrixSize = std::clamp(size, 2, 4);
    p.values.assign(16, 0.0f);
    for (int i = 0; i < 4; ++i) p.values[static_cast<size_t>(i * 4 + i)] = 1.0f;
    return p;
}

float Param::evalCurve(double time) const {
    if (keys.empty()) return value;
    if (keys.size() == 1) return keys.front().value;
    if (time <= keys.front().time) return keys.front().value;
    if (time >= keys.back().time) return keys.back().value;
    for (size_t i = 1; i < keys.size(); ++i) {
        if (time <= keys[i].time) {
            const Keyframe &a = keys[i - 1];
            const Keyframe &b = keys[i];
            const double span = b.time - a.time;
            if (span <= 1e-9) return b.value;
            const float t = static_cast<float>((time - a.time) / span);
            // Easing ids: 0 linear, 1 ease-in, 2 ease-out, 3 smooth, 4 step
            switch (b.easing) {
                case 1: return lerp(a.value, b.value, t * t);
                case 2: return lerp(a.value, b.value, 1.0f - (1.0f - t) * (1.0f - t));
                case 3: return lerp(a.value, b.value, t * t * (3.0f - 2.0f * t));
                case 4: return a.value;
                default: return lerp(a.value, b.value, t);
            }
        }
    }
    return keys.back().value;
}

Color colorFromHsv(float h, float s, float v, float a) {
    h = h - std::floor(h);
    s = std::clamp(s, 0.0f, 1.0f);
    v = std::clamp(v, 0.0f, 1.0f);
    const float i = std::floor(h * 6.0f);
    const float f = h * 6.0f - i;
    const float p = v * (1.0f - s);
    const float q = v * (1.0f - f * s);
    const float t = v * (1.0f - (1.0f - f) * s);
    float r = 0, g = 0, b = 0;
    switch (static_cast<int>(i) % 6) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
    }
    return Color{static_cast<unsigned char>(std::lround(r * 255.0f)),
                 static_cast<unsigned char>(std::lround(g * 255.0f)),
                 static_cast<unsigned char>(std::lround(b * 255.0f)),
                 static_cast<unsigned char>(std::lround(std::clamp(a, 0.0f, 1.0f) * 255.0f))};
}

Color paramColorAt(const Param &param) { return param.color; }

void colorToHsv(Color color, float *h, float *s, float *v) {
    const float r = color.r / 255.0f;
    const float g = color.g / 255.0f;
    const float b = color.b / 255.0f;
    const float maxValue = std::max({r, g, b});
    const float minValue = std::min({r, g, b});
    const float delta = maxValue - minValue;

    float hue = 0.0f;
    if (delta > 1e-6f) {
        if (maxValue == r) {
            hue = std::fmod((g - b) / delta, 6.0f);
        } else if (maxValue == g) {
            hue = (b - r) / delta + 2.0f;
        } else {
            hue = (r - g) / delta + 4.0f;
        }
        hue /= 6.0f;
        if (hue < 0.0f) hue += 1.0f;
    }
    if (h) *h = hue;
    if (s) *s = maxValue > 1e-6f ? delta / maxValue : 0.0f;
    if (v) *v = maxValue;
}

}  // namespace pf
