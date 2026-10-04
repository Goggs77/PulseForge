#include "render/ShaderLibrary.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

namespace pf {

// ---------------------------------------------------------------------------
// Shared shader source
// ---------------------------------------------------------------------------

const char *ShaderLibrary::vertexSource() {
    return R"(#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec4 vertexColor;
uniform mat4 mvp;
out vec2 fragTexCoord;
out vec4 fragColor;
void main() {
    fragTexCoord = vertexTexCoord;
    fragColor = vertexColor;
    gl_Position = mvp*vec4(vertexPosition, 1.0);
}
)";
}

std::string ShaderLibrary::preamble() {
    return R"(
// ---- PulseForge shader preamble -------------------------------------------
// fragTexCoord: (0,0) at the top-left, (1,1) at the bottom-right.
in vec2 fragTexCoord;
in vec4 fragColor;
out vec4 finalColor;

uniform sampler2D texture0;    // the quad's own texture (raylib batch)
uniform vec4  colDiffuse;

uniform sampler2D uPrev;       // upstream Image input
uniform sampler2D uInput2;     // second Image input (blend/posts)
uniform sampler2D uFeedback;   // this block's previous frame
uniform sampler2D uSpectrum;   // 1D spectrum, x = log frequency, r = magnitude
uniform sampler2D uWaveform;   // 1D waveform around the playhead, r = 0.5+v/2

uniform vec2  uResolution;
uniform vec2  uTexel;          // 1.0 / uResolution
uniform float uTime;           // seconds
uniform float uFrame;
uniform float uDuration;
uniform float uProgress;       // uTime / uDuration, clamped to 0..1

uniform float uBass;
uniform float uMid;
uniform float uTreble;
uniform float uLevel;
uniform float uOnset;
uniform float uBeat;

uniform float uUser[8];        // scalar inputs, in port order
uniform vec4  uColorA;
uniform vec4  uColorB;
uniform vec2  uVector2;        // Vector2 port
uniform vec3  uVector3;        // Vector3 port
uniform vec4  uVector4;        // Vector4 port
uniform mat4  uMatrix;         // Matrix port

vec2  pfUv() { return fragTexCoord; }
vec4  pfPrev(vec2 uv) { return texture(uPrev, uv); }
vec4  pfInput2(vec2 uv) { return texture(uInput2, uv); }
vec4  pfFeedback(vec2 uv) { return texture(uFeedback, uv); }
float pfSpectrum(float x) { return texture(uSpectrum, vec2(clamp(x, 0.0, 1.0), 0.5)).r; }
float pfSpectrumBand(float lo, float hi) {
    float sum = 0.0;
    for (int i = 0; i < 8; ++i) sum += pfSpectrum(mix(lo, hi, (float(i) + 0.5)/8.0));
    return sum/8.0;
}
float pfWave(float x) { return texture(uWaveform, vec2(clamp(x, 0.0, 1.0), 0.5)).r*2.0 - 1.0; }

vec2 pfRotate(vec2 p, float a) {
    float c = cos(a), s = sin(a);
    return vec2(c*p.x - s*p.y, s*p.x + c*p.y);
}
// Spectrum presets drive their own parameters through the uUser slots. When
// the same built-in is used through a Shader block the slot is 0, so a
// positive-only parameter falls back to the value the effect had before it
// became adjustable.
float pfParam(float value, float fallback) { return value > 0.0001 ? value : fallback; }
float pfHash(vec2 p) {
    p = fract(p*vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x*p.y);
}
float pfNoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f*f*(3.0 - 2.0*f);
    float a = pfHash(i), b = pfHash(i + vec2(1.0, 0.0));
    float c = pfHash(i + vec2(0.0, 1.0)), d = pfHash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
float pfFbm(vec2 p) {
    float v = 0.0, a = 0.5;
    for (int i = 0; i < 5; ++i) { v += a*pfNoise(p); p *= 2.0; a *= 0.5; }
    return v;
}
vec3 pfHsv2Rgb(vec3 c) {
    vec3 rgb = clamp(abs(mod(c.x*6.0 + vec3(0.0, 4.0, 2.0), 6.0) - 3.0) - 1.0, 0.0, 1.0);
    return c.z * mix(vec3(1.0), rgb, c.y);
}
// ---- end preamble ---------------------------------------------------------
)";
}

// ---------------------------------------------------------------------------
// Built-in effects
// ---------------------------------------------------------------------------

namespace {
struct BuiltinShader {
    const char *name;
    const char *body;
};

const BuiltinShader kBuiltins[] = {
    {"passthrough", R"(
void main() { finalColor = vec4(pfPrev(pfUv()).rgb, 1.0); }
)"},
    {"solid", R"(
void main() { finalColor = vec4(uColorA.rgb, 1.0); }
)"},
    {"gradient", R"(
void main() {
    vec2 uv = pfUv();
    float speed = pfParam(uUser[0], 1.0);
    float t = uTime*0.15*speed + uv.y*0.6;
    vec3 col = mix(uColorA.rgb, uColorB.rgb, 0.5 + 0.5*sin(t*3.14159));
    col += 0.12*pfFbm(uv*3.0 + uTime*0.2*speed);
    col *= 0.75 + 0.5*uLevel;
    finalColor = vec4(col, 1.0);
}
)"},
    {"plasma", R"(
void main() {
    vec2 uv = (pfUv() - 0.5)*vec2(uResolution.x/uResolution.y, 1.0);
    float r = length(uv);
    float a = atan(uv.y, uv.x);
    float speed = pfParam(uUser[0], 1.0);
    float complexity = pfParam(uUser[1], 1.0);
    float n = pfFbm(uv*2.5*complexity + vec2(uTime*0.10, uTime*0.07)*speed);
    float v = sin(6.0*r - uTime*1.2*speed) + sin(a*4.0 + uTime*0.7*speed) + n*1.6;
    v += uBass*2.2 + uMid*0.9;
    vec3 col = pfHsv2Rgb(vec3(fract(0.55 + v*0.06 + uTreble*0.25), 0.75, 0.55 + 0.35*tanh(v*0.5)));
    col = mix(col, uColorB.rgb, 0.25);
    finalColor = vec4(col, 1.0);
}
)"},
    {"radial_spectrum", R"(
void main() {
    vec2 uv = pfUv();
    vec2 p = (uv - 0.5)*vec2(uResolution.x/uResolution.y, 1.0);
    float r = length(p);
    // Rotation is in turns, so the bands can be spun around the centre.
    float a = atan(p.y, p.x) + uUser[0]*6.2831853;
    float band = fract((a/6.2831853) + 0.5);
    float mag = pfSpectrum(pow(band, 0.75));
    float inner = 0.16 + 0.25*mag + 0.05*uLevel;
    float outer = inner + 0.05 + 0.35*mag;
    float ring = smoothstep(inner, inner + 0.012, r) - smoothstep(outer, outer + 0.012, r);
    vec3 col = mix(uColorA.rgb, uColorB.rgb, mag);
    col *= 0.6 + 1.6*ring;
    col += 0.25*ring*uOnset;
    col += 0.05*pfPrev(uv).rgb;
    finalColor = vec4(col, 1.0);
}
)"},
    {"bars", R"(
void main() {
    vec2 uv = pfUv();
    float bars = floor(clamp(pfParam(uUser[0], 72.0), 4.0, 256.0));
    float height = pfParam(uUser[1], 0.85);
    float i = floor(uv.x*bars);
    float f = fract(uv.x*bars);
    float mag = pfSpectrum(i/bars);
    float h = 0.05 + height*mag;
    float x = smoothstep(0.06, 0.16, f)*smoothstep(0.94, 0.84, f);
    float bar = step(1.0 - h, uv.y)*x;
    vec3 col = mix(uColorA.rgb, uColorB.rgb, mag);
    col *= bar;
    col += 0.2*uLevel*pfPrev(uv).rgb;
    finalColor = vec4(col, 1.0);
}
)"},
    {"waveform_scope", R"(
void main() {
    vec2 uv = pfUv();
    float amplitude = pfParam(uUser[0], 1.0);
    float thickness = pfParam(uUser[1], 1.0);
    float w = pfWave(uv.x);
    float y = 0.5 + w*0.28*amplitude;
    float d = abs(uv.y - y);
    float glow = exp(-d*90.0/thickness) + 0.35*exp(-d*22.0/thickness);
    vec3 col = mix(uColorA.rgb, uColorB.rgb, 0.5 + 0.5*w);
    col *= glow*(0.8 + 1.4*uLevel);
    col += 0.06*pfPrev(uv).rgb;
    finalColor = vec4(col, 1.0);
}
)"},
    {"spectrogram", R"(
void main() {
    vec2 uv = pfUv();
    float brightness = pfParam(uUser[0], 1.0);
    float bin = pow(uv.y, 1.6);
    float mag = pfSpectrum(bin);
    mag = pow(mag, 0.7);
    vec3 col = mix(uColorA.rgb, uColorB.rgb, clamp(mag*1.3, 0.0, 1.0));
    col *= mag*2.2*brightness;
    col += 0.04*pfPrev(uv).rgb;
    finalColor = vec4(col, 1.0);
}
)"},
    {"tunnel", R"(
void main() {
    vec2 uv = pfUv();
    vec2 p = uv - 0.5;
    float r = max(length(p), 0.001);
    float a = atan(p.y, p.x);
    float speed = pfParam(uUser[0], 1.0);
    float twist = pfParam(uUser[1], 1.0);
    float depth = 0.35/r;
    a += 0.6*sin(depth*0.4 + uTime*0.6*speed)*twist + uBass*0.6;
    float u = a/3.14159265;
    float v = fract(depth*0.25 - uTime*0.35*speed);
    vec3 pat = vec3(pfNoise(vec2(u*3.0, v*6.0)));
    vec3 col = mix(uColorA.rgb, uColorB.rgb, pat.r);
    col *= smoothstep(0.0, 0.35, r);
    col += 0.5*uOnset;
    col += 0.35*pfFeedback(vec2(u*0.5 + 0.5, v)).rgb;
    finalColor = vec4(col, 1.0);
}
)"},
    {"kaleidoscope", R"(
void main() {
    vec2 uv = pfUv() - 0.5;
    float a = atan(uv.y, uv.x);
    float r = length(uv);
    float segments = floor(clamp(pfParam(uUser[0], 6.0), 2.0, 32.0));
    float speed = pfParam(uUser[1], 1.0);
    float seg = 6.2831853/segments;
    a = abs(mod(a + uTime*0.1*speed, seg) - seg*0.5);
    vec2 q = vec2(cos(a), sin(a))*r + 0.5;
    vec3 col = pfPrev(q).rgb;
    col *= 0.7 + 0.6*uLevel;
    col += 0.10*pfFbm(q*4.0 + uTime*0.2);
    finalColor = vec4(col, 1.0);
}
)"},
    {"starfield", R"(
void main() {
    vec2 uv = (pfUv() - 0.5)*vec2(uResolution.x/uResolution.y, 1.0);
    vec3 col = vec3(0.0);
    for (int i = 0; i < 24; ++i) {
        float fi = float(i);
        float seed = pfHash(vec2(fi, 1.0));
        vec2 dir = normalize(vec2(pfHash(vec2(fi, 2.0)) - 0.5, pfHash(vec2(fi, 3.0)) - 0.5) + 0.001);
        float speed = (0.15 + 0.7*pfHash(vec2(fi, 4.0)) + uBass*0.6)*pfParam(uUser[0], 1.0);
        float z = fract(seed + uTime*speed*0.12);
        vec2 pos = dir*(0.15 + z*1.2);
        float d = length(uv - pos);
        float s = 0.004 + 0.02*(1.0 - z)*uTreble;
        col += exp(-d/max(s, 0.001))*mix(uColorA.rgb, uColorB.rgb, seed)*
               pfParam(uUser[1], 1.0);
    }
    col += 0.05*pfPrev(pfUv()).rgb;
    finalColor = vec4(col, 1.0);
}
)"},
    {"bloom", R"(
void main() {
    vec2 uv = pfUv();
    vec3 base = pfPrev(uv).rgb;
    float amount = max(uUser[0], 0.0);
    float threshold = pfParam(uUser[1], 0.35);
    vec3 sum = vec3(0.0);
    float total = 0.0;
    for (int i = 0; i < 12; ++i) {
        float a = float(i)*0.5235988;
        vec2 o = vec2(cos(a), sin(a))*uTexel*(6.0 + 10.0*float(i)/12.0);
        float w = 1.0/(1.0 + float(i));
        sum += pfPrev(uv + o).rgb*w;
        total += w;
    }
    vec3 blur = sum/total;
    vec3 bright = max(blur - vec3(threshold), vec3(0.0));
    finalColor = vec4(base + bright*amount, 1.0);
}
)"},
    {"chromatic", R"(
void main() {
    vec2 uv = pfUv();
    vec2 d = (uv - 0.5);
    float amount = max(uUser[0], 0.0);
    float k = (0.004 + 0.06*uLevel)*amount;
    vec3 col;
    col.r = pfPrev(uv + d*k*1.2).r;
    col.g = pfPrev(uv).g;
    col.b = pfPrev(uv - d*k*1.2).b;
    float vig = smoothstep(1.15, 0.25, length(d)*1.6);
    finalColor = vec4(col*vig, 1.0);
}
)"},
    {"feedback_trail", R"(
void main() {
    vec2 uv = pfUv();
    vec3 cur = pfPrev(uv).rgb;
    vec3 prev = pfFeedback(uv).rgb;
    // Without a driven Amount (the effect loaded through a Shader block) the
    // trail follows the loudness, which is how this preset behaved before.
    float amount = clamp(uUser[0] > 0.0001 ? uUser[0] : (0.72 + 0.25*uLevel), 0.0, 0.98);
    vec3 col = mix(cur, prev, amount);
    col *= 0.995;
    finalColor = vec4(col, 1.0);
}
)"},
    {"vignette", R"(
void main() {
    vec2 uv = pfUv();
    vec3 col = pfPrev(uv).rgb;
    float d = length(uv - 0.5);
    float amount = clamp(max(uUser[0], 0.0), 0.0, 1.0);
    float softness = pfParam(uUser[1], 1.0);
    col *= mix(1.0, smoothstep(0.95*softness, 0.25*softness, d), amount);
    col += 0.15*uOnset;
    finalColor = vec4(col, 1.0);
}
)"},
    // --- composite helpers used by fx.blend / fx.postfx -------------------
    {"blend", R"(
void main() {
    vec2 uv = pfUv();
    vec4 a = pfPrev(uv);
    vec4 b = pfInput2(uv);
    float op = clamp(uUser[1], 0.0, 1.0);
    int mode = int(uUser[0] + 0.5);
    vec3 o;
    if (mode == 0) o = mix(a.rgb, b.rgb, op);
    else if (mode == 1) o = a.rgb + b.rgb*op;
    else if (mode == 2) o = 1.0 - (1.0 - a.rgb)*(1.0 - b.rgb*op);
    else if (mode == 3) o = a.rgb*(1.0 - op + b.rgb*op);
    else if (mode == 4) o = abs(a.rgb - b.rgb*op);
    else if (mode == 5) o = mix(a.rgb, 2.0*a.rgb*b.rgb, op);
    else if (mode == 6) o = min(a.rgb, b.rgb);
    else o = max(a.rgb, b.rgb);
    finalColor = vec4(o, 1.0);
}
)"},
    {"postfx", R"(
void main() {
    vec2 uv = pfUv();
    vec2 d = uv - 0.5;
    float bloomAmount = clamp(uUser[0], 0.0, 1.0);
    float chroma      = clamp(uUser[1], 0.0, 1.0);
    float vignette    = clamp(uUser[2], 0.0, 1.0);
    float grain       = clamp(uUser[3], 0.0, 1.0);
    float scan        = clamp(uUser[4], 0.0, 1.0);
    float feedback    = clamp(uUser[5], 0.0, 0.98);
    float saturation  = uUser[6];
    float hue         = uUser[7];

    float k = 0.002 + 0.03*chroma;
    vec3 col;
    col.r = pfPrev(uv + d*k*1.4).r;
    col.g = pfPrev(uv).g;
    col.b = pfPrev(uv - d*k*1.4).b;

    if (bloomAmount > 0.001) {
        vec3 sum = vec3(0.0); float total = 0.0;
        for (int i = 0; i < 10; ++i) {
            float a = float(i)*0.6283185;
            vec2 o = vec2(cos(a), sin(a))*uTexel*(5.0 + 12.0*float(i)/10.0);
            float w = 1.0/(1.0 + float(i));
            sum += pfPrev(uv + o).rgb*w;
            total += w;
        }
        vec3 blur = sum/total;
        col += max(blur - 0.30, 0.0)*bloomAmount*2.0;
    }

    if (feedback > 0.001) col = mix(col, pfFeedback(uv).rgb, feedback);

    col = mix(vec3(dot(col, vec3(0.299, 0.587, 0.114))), col, clamp(saturation, 0.0, 2.0));
    if (abs(hue) > 0.0001) {
        float ang = dot(col, vec3(0.577, 0.577, 0.577));
        col = mix(vec3(ang), col, 1.0) ;
        col.rb = mix(col.rb, col.br, abs(hue)*0.5);
    }
    if (vignette > 0.001) col *= mix(1.0, smoothstep(1.05, 0.25, length(d)*1.5), vignette);
    if (scan > 0.001) col *= 1.0 - scan*0.35*step(0.5, fract(uv.y*uResolution.y*0.5));
    if (grain > 0.001) col += (pfHash(uv*uResolution + uTime*57.0) - 0.5)*grain*0.14;
    finalColor = vec4(col, 1.0);
}
)"},
};
}  // namespace

// ---------------------------------------------------------------------------
// ShaderLibrary
// ---------------------------------------------------------------------------

ShaderLibrary::~ShaderLibrary() { reset(); }

void ShaderLibrary::reset() {
    for (auto &kv : entries_) {
        if (kv.second.valid) UnloadShader(kv.second.shader);
    }
    entries_.clear();
}

void ShaderLibrary::setSearchPaths(const std::vector<std::string> &paths) { searchPaths_ = paths; }

bool ShaderLibrary::isBuiltinReference(const std::string &reference) {
    return reference.rfind("builtin:", 0) == 0 || reference.rfind("builtin ", 0) == 0;
}

std::string ShaderLibrary::builtinNameFromReference(const std::string &reference) {
    const size_t colon = reference.find(':');
    if (colon == std::string::npos) return std::string();
    std::string name = reference.substr(colon + 1);
    while (!name.empty() && (name.front() == ' ' || name.front() == '/')) name.erase(name.begin());
    while (!name.empty() && name.back() == ' ') name.pop_back();
    return name;
}

std::vector<std::string> ShaderLibrary::builtinNames() {
    std::vector<std::string> names;
    for (const auto &entry : kBuiltins) names.emplace_back(entry.name);
    return names;
}

std::vector<std::string> ShaderLibrary::effectNames() {
    std::vector<std::string> names;
    for (const auto &entry : kBuiltins) {
        // The last entries are the composite helpers other blocks call directly.
        if (std::strcmp(entry.name, "blend") == 0 || std::strcmp(entry.name, "postfx") == 0) {
            continue;
        }
        names.emplace_back(entry.name);
    }
    return names;
}

std::vector<ShaderInput> ShaderLibrary::scanInputs(const std::string &source) {
    std::vector<ShaderInput> inputs;
    auto uses = [&](const char *token) { return source.find(token) != std::string::npos; };

    // Scalar slots keep their uUser[i] numbering so links survive a shader swap.
    std::vector<int> userSlots;
    size_t pos = 0;
    while ((pos = source.find("uUser[", pos)) != std::string::npos) {
        size_t cursor = pos + 6;
        int index = 0;
        bool digits = false;
        while (cursor < source.size() && source[cursor] >= '0' && source[cursor] <= '9') {
            index = index * 10 + (source[cursor] - '0');
            digits = true;
            ++cursor;
        }
        if (digits && cursor < source.size() && source[cursor] == ']' && index >= 0 && index < 8) {
            if (std::find(userSlots.begin(), userSlots.end(), index) == userSlots.end()) {
                userSlots.push_back(index);
            }
        }
        pos += 6;
    }
    std::sort(userSlots.begin(), userSlots.end());

    inputs.push_back(ShaderInput{"uPrev", "uPrev", PortType::Image});
    if (uses("uInput2")) inputs.push_back(ShaderInput{"uInput2", "uInput2", PortType::Image});
    for (int index : userSlots) {
        const std::string label = "uUser[" + std::to_string(index) + "]";
        inputs.push_back(ShaderInput{label, label, PortType::Scalar});
    }
    if (uses("uColorA")) inputs.push_back(ShaderInput{"uColorA", "uColorA", PortType::Color});
    if (uses("uColorB")) inputs.push_back(ShaderInput{"uColorB", "uColorB", PortType::Color});
    if (uses("uVector2")) inputs.push_back(ShaderInput{"uVector2", "uVector2", PortType::Vec2});
    if (uses("uVector3")) inputs.push_back(ShaderInput{"uVector3", "uVector3", PortType::Vec3});
    if (uses("uVector4")) inputs.push_back(ShaderInput{"uVector4", "uVector4", PortType::Vec4});
    if (uses("uMatrix")) inputs.push_back(ShaderInput{"uMatrix", "uMatrix", PortType::Matrix});
    return inputs;
}

bool ShaderLibrary::readSource(const std::string &reference, std::string *source,
                               std::string *error) const {
    if (source) source->clear();
    if (reference.empty()) {
        if (error) *error = "no shader selected";
        return false;
    }
    if (isBuiltinReference(reference)) {
        const std::string name = builtinNameFromReference(reference);
        for (const auto &entry : kBuiltins) {
            if (name == entry.name) {
                if (source) *source = entry.body;
                return true;
            }
        }
        if (error) *error = "unknown built-in shader: " + name;
        return false;
    }
    const std::string path = resolve(reference);
    std::ifstream file(path.c_str(), std::ios::binary);
    if (!file.good()) {
        if (error) *error = "cannot open shader: " + path;
        return false;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    if (source) *source = buffer.str();
    return true;
}

bool ShaderLibrary::describeInputs(const std::string &reference, std::vector<ShaderInput> *inputs,
                                   std::string *error) const {
    std::string source;
    if (!readSource(reference, &source, error)) return false;
    if (inputs) *inputs = scanInputs(source);
    return true;
}

long long ShaderLibrary::fileMtime(const std::string &path) {
    struct stat info;
    if (stat(path.c_str(), &info) != 0) return 0;
    return static_cast<long long>(info.st_mtime);
}

unsigned long long ShaderLibrary::hashString(const std::string &text) {
    unsigned long long hash = 1469598103934665603ull;
    for (unsigned char c : text) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string ShaderLibrary::resolve(const std::string &reference) const {
    if (reference.empty() || isBuiltinReference(reference)) return reference;
    std::ifstream probe(reference.c_str());
    if (probe.good()) return reference;
    for (const auto &dir : searchPaths_) {
        if (dir.empty()) continue;
        std::string candidate = dir;
        if (candidate.back() != '/' && candidate.back() != '\\') candidate += '/';
        candidate += reference;
        std::ifstream file(candidate.c_str());
        if (file.good()) return candidate;
    }
    return reference;
}

bool ShaderLibrary::compile(const std::string &reference, const std::string &source, Entry &entry) {
    Shader shader = LoadShaderFromMemory(vertexSource(), source.c_str());
    if (!IsShaderValid(shader)) {
        entry.error = "GLSL compilation failed for " + reference;
        // raylib already traced the compiler log; keep a pointer for the UI.
        TraceLog(LOG_WARNING, "PulseForge: %s", entry.error.c_str());
        return false;
    }
    entry.shader = shader;
    entry.valid = true;
    entry.error.clear();
    return true;
}

Shader *ShaderLibrary::builtin(const std::string &name) {
    std::string key = "builtin:" + name;
    auto it = entries_.find(key);
    if (it != entries_.end() && it->second.valid) return &it->second.shader;

    const BuiltinShader *found = nullptr;
    for (const auto &entry : kBuiltins) {
        if (name == entry.name) {
            found = &entry;
            break;
        }
    }
    if (!found) {
        lastError_ = "unknown built-in shader: " + name;
        return nullptr;
    }
    std::string source = "#version 330\n";
    source += preamble();
    source += found->body;
    Entry entry;
    if (!compile(key, source, entry)) {
        lastError_ = entry.error;
        return nullptr;
    }
    entries_[key] = std::move(entry);
    return &entries_[key].shader;
}

Shader *ShaderLibrary::get(const std::string &reference, std::string *error) {
    if (reference.empty()) {
        if (error) *error = "no shader selected";
        return nullptr;
    }
    if (isBuiltinReference(reference)) {
        Shader *shader = builtin(builtinNameFromReference(reference));
        if (!shader && error) *error = lastError_;
        return shader;
    }

    const std::string path = resolve(reference);
    const long long mtime = fileMtime(path);
    if (mtime == 0) {
        if (error) *error = "shader file not found: " + reference;
        return nullptr;
    }

    auto it = entries_.find(path);
    if (it != entries_.end() && it->second.valid && it->second.mtime == mtime) {
        return &it->second.shader;
    }
    if (it != entries_.end() && it->second.valid) {
        UnloadShader(it->second.shader);
    }

    std::ifstream file(path.c_str(), std::ios::binary);
    if (!file.good()) {
        if (error) *error = "cannot open shader: " + path;
        return nullptr;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string body = buffer.str();

    std::string source;
    // A file that declares its own version is used verbatim; otherwise the
    // PulseForge preamble is prepended so shaders stay short.
    const size_t firstNonSpace = body.find_first_not_of(" \t\r\n");
    const bool selfContained = firstNonSpace != std::string::npos &&
                               body.compare(firstNonSpace, 8, "#version") == 0;
    if (selfContained) {
        source = body;
    } else {
        source = "#version 330\n";
        source += preamble();
        source += body;
    }

    Entry entry;
    entry.resolvedPath = path;
    entry.mtime = mtime;
    if (!compile(path, source, entry)) {
        if (error) *error = entry.error;
        entries_[path] = std::move(entry);
        return nullptr;
    }
    entries_[path] = std::move(entry);
    return &entries_[path].shader;
}

int ShaderLibrary::reloadChanged() {
    std::vector<std::string> stale;
    for (const auto &kv : entries_) {
        if (kv.second.resolvedPath.empty()) continue;
        const long long mtime = fileMtime(kv.second.resolvedPath);
        if (mtime > 0 && mtime != kv.second.mtime) stale.push_back(kv.second.resolvedPath);
    }
    int reloaded = 0;
    for (const std::string &path : stale) {
        std::string error;
        get(path, &error);
        ++reloaded;
    }
    return reloaded;
}

}  // namespace pf
