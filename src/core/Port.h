// Port types, values flowing between blocks, and the parameter model shared by
// the inspector, the pipeline evaluator and project serialisation.
#pragma once

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "raylib.h"

namespace pf {

// ---------------------------------------------------------------------------
// Port types
// ---------------------------------------------------------------------------

enum class PortType : int {
    None = 0,
    Audio,     // a decoded clip
    Analysis,  // per-frame spectral analysis
    Scalar,    // a single float, the currency of modulation
    Color,
    Image,     // a GPU render target
    Text,
    Vec2,      // 2D vector
    Vec3,      // 3D vector
    Vec4,      // 4D vector
    Matrix,    // 4x4 matrix
};

const char *portTypeName(PortType type);
Color portTypeColor(PortType type);

// A source port may feed a target port when the types match. Vectors may also
// widen (Vector2 -> Vector3 -> Vector4) with the missing components filled in
// with zero, which keeps the matrix and vector blocks composable.
inline bool portTypeCompatible(PortType from, PortType to) {
    if (from == to) return true;
    if (to == PortType::Vec3 && from == PortType::Vec2) return true;
    if (to == PortType::Vec4 && (from == PortType::Vec2 || from == PortType::Vec3)) return true;
    return false;
}


// ---------------------------------------------------------------------------
// Payloads
// ---------------------------------------------------------------------------

struct AudioBuffer {
    std::vector<float> samples;  // interleaved
    int channels = 1;
    int sampleRate = 48000;
    long long frameCount = 0;  // samples per channel

    double duration() const {
        return sampleRate > 0 ? static_cast<double>(frameCount) / sampleRate : 0.0;
    }

    // Downmixed sample at a frame position, linearly interpolated.
    float monoAt(double frame) const;
    float mono(int frame) const { return monoAt(frame); }
};

struct AnalysisFrame {
    float rms = 0.0f;
    float level = 0.0f;  // smoothed RMS, 0..1
    float flux = 0.0f;
    float onset = 0.0f;  // 0..1 transient strength
    std::vector<float> bands;  // bandCount magnitudes, 0..1
};

struct AnalysisData {
    int sampleRate = 48000;
    int fftSize = 2048;
    int hopSize = 512;
    int bandCount = 64;
    int spectrumBins = 256;  // log-spaced bins kept for the GPU texture
    double duration = 0.0;
    std::vector<AnalysisFrame> frames;
    // frames.size() * spectrumBins magnitudes, normalised to 0..1
    std::vector<float> spectrum;
    double computeSeconds = 0.0;

    double frameTime(size_t index) const {
        return static_cast<double>(index) * hopSize / sampleRate;
    }
    size_t frameIndexAt(double time) const;
    const AnalysisFrame *frameAt(double time) const;
    float bandAt(double time, int band) const;
    // kind: 0 = rms, 1 = level, 2 = flux, 3 = onset
    float valueAt(double time, int kind) const;
    const float *spectrumRow(double time) const;
};

// GPU render target handed between Image ports. Owned by the Renderer's target
// pool; the shared_ptr keeps it alive while a frame is being evaluated.
struct ImageBuffer {
    RenderTexture2D texture{};
    int width = 0;
    int height = 0;
    bool valid() const { return texture.id != 0; }
};
using ImageBufferPtr = std::shared_ptr<ImageBuffer>;
using AudioPtr = std::shared_ptr<const AudioBuffer>;
using AnalysisPtr = std::shared_ptr<const AnalysisData>;

struct Value {
    PortType type = PortType::None;
    float scalar = 0.0f;
    Color color = WHITE;
    std::string text;
    Vector2 vec2{};
    Vector3 vec3{};
    Vector4 vec4{};
    Matrix matrix{};
    AudioPtr audio;
    AnalysisPtr analysis;
    ImageBufferPtr image;

    static Value makeScalar(float v) {
        Value out;
        out.type = PortType::Scalar;
        out.scalar = v;
        return out;
    }
    static Value makeColor(Color c) {
        Value out;
        out.type = PortType::Color;
        out.color = c;
        return out;
    }
    static Value makeVec2(Vector2 v) {
        Value out;
        out.type = PortType::Vec2;
        out.vec2 = v;
        return out;
    }
    static Value makeVec3(Vector3 v) {
        Value out;
        out.type = PortType::Vec3;
        out.vec3 = v;
        return out;
    }
    static Value makeVec4(Vector4 v) {
        Value out;
        out.type = PortType::Vec4;
        out.vec4 = v;
        return out;
    }
    static Value makeMatrix(Matrix m) {
        Value out;
        out.type = PortType::Matrix;
        out.matrix = m;
        return out;
    }
    static Value makeImage(ImageBufferPtr img) {
        Value out;
        out.type = PortType::Image;
        out.image = std::move(img);
        return out;
    }
    float asScalar(float fallback = 0.0f) const {
        return type == PortType::Scalar ? scalar : fallback;
    }
};

// Converts a value for a target port, widening vectors and padding with zero,
// and producing an empty value when the types are not compatible.
Value convertValue(const Value &source, PortType target);
// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------

struct Keyframe {
    double time = 0.0;
    float value = 0.0f;
    int easing = 0;  // index into the easing table in widgets
};

enum class ParamKind : int {
    Float = 0,
    Int,
    Bool,
    Color,
    Text,
    Enum,
    File,
    Curve,   // keyframed automation
    Wave,    // LFO shape selector
    Band,    // frequency band selector
    Matrix,  // NxN numeric grid edited as input boxes
};

struct Param {
    std::string key;
    std::string label;
    ParamKind kind = ParamKind::Float;
    std::string group;

    float value = 0.0f;      // Float / Int
    float minValue = 0.0f;
    float maxValue = 1.0f;
    float step = 0.0f;       // 0 = continuous
    // Frequency-style parameters: the slider tracks logarithmically and the
    // value readout is a rounded integer, which matches how the analysis
    // itself maps Hz.
    bool logarithmic = false;

    bool boolean = false;
    Color color = WHITE;
    std::string text;                    // Text / File / Enum
    std::vector<std::string> options;    // Enum
    std::vector<Keyframe> keys;          // Curve
    std::vector<float> values;           // Matrix grid (row major, 16 entries)
    int matrixSize = 4;                  // active NxN sub-grid for Matrix params
    std::string hint;                    // File extension filter, e.g. ".glsl"

    int intValue() const { return static_cast<int>(std::lround(value)); }
    void setInt(int v) { value = static_cast<float>(v); }
    float normalized() const {
        return (maxValue > minValue) ? (value - minValue) / (maxValue - minValue) : 0.0f;
    }
    void setNormalized(float t) { value = minValue + std::clamp(t, 0.0f, 1.0f) * (maxValue - minValue); }
    // Evaluates the Curve kind at the given time.
    float evalCurve(double time) const;
};

Param makeParam(const std::string &key, const std::string &label, float value, float minValue,
                float maxValue, float step = 0.0f, const std::string &group = std::string(),
                bool logarithmic = false);
Param makeIntParam(const std::string &key, const std::string &label, int value, int minValue,
                   int maxValue, const std::string &group = std::string());
Param makeBoolParam(const std::string &key, const std::string &label, bool value,
                    const std::string &group = std::string());
Param makeColorParam(const std::string &key, const std::string &label, Color value,
                     const std::string &group = std::string());
Param makeEnumParam(const std::string &key, const std::string &label,
                    const std::vector<std::string> &options, int index,
                    const std::string &group = std::string());
Param makeTextParam(const std::string &key, const std::string &label, const std::string &value,
                    const std::string &group = std::string());
Param makeFileParam(const std::string &key, const std::string &label, const std::string &value,
                    const std::string &extension, const std::string &group = std::string());
Param makeCurveParam(const std::string &key, const std::string &label);
Param makeMatrixParam(const std::string &key, const std::string &label, int size);

// Row-major accessor for Matrix params.
inline float matrixParamValue(const Param &param, int row, int column) {
    const size_t index = static_cast<size_t>(row) * 4u + static_cast<size_t>(column);
    return index < param.values.size() ? param.values[index] : (row == column ? 1.0f : 0.0f);
}

// ---------------------------------------------------------------------------
// Small shared helpers
// ---------------------------------------------------------------------------

// 0..1 soft clip, useful for signal shaping.
inline float softClip(float x) { return x / (1.0f + std::fabs(x)); }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
inline float normalize01(float value, float lo, float hi) {
    if (hi <= lo) return 0.0f;
    return std::clamp((value - lo) / (hi - lo), 0.0f, 1.0f);
}
Color colorFromHsv(float h, float s, float v, float a = 1.0f);
// Inverse of colorFromHsv; h, s and v are returned in 0..1.
void colorToHsv(Color color, float *h, float *s, float *v);
Color paramColorAt(const Param &param);

}  // namespace pf
