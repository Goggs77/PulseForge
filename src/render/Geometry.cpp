#include "render/Geometry.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

#include "core/Port.h"
#include "render/Palette.h"

namespace pf::geometry {

const char *shapeName(Shape shape) {
    switch (shape) {
        case Shape::Circle: return "Circle";
        case Shape::Ring: return "Ring";
        case Shape::RadialBars: return "Radial Bars";
        case Shape::BarSpectrum: return "Bar Spectrum";
        case Shape::WaveformRing: return "Waveform Ring";
        case Shape::WaveformLine: return "Waveform Line";
        case Shape::PolygonGrid: return "Polygon Grid";
        case Shape::Sparks: return "Sparks";
        case Shape::Orbit: return "Orbit";
        case Shape::TextOverlay: return "Text";
        default: return "None";
    }
}

std::vector<std::string> shapeNames() {
    // Index in this list equals the Shape enum value, so a parameter index can
    // be cast straight to Shape.
    std::vector<std::string> names;
    for (int i = 0; i < static_cast<int>(Shape::Count); ++i) {
        names.emplace_back(shapeName(static_cast<Shape>(i)));
    }
    return names;
}

namespace {

struct Particle {
    Vector2 position{};
    Vector2 velocity{};
    float life = 0.0f;
    float maxLife = 1.0f;
    float size = 2.0f;
    float hue = 0.0f;
};

std::unordered_map<int, std::vector<Particle>> gParticles;
unsigned int gRandomState = 0x1234567u;

float frand() {
    gRandomState = gRandomState * 1664525u + 1013904223u;
    return static_cast<float>((gRandomState >> 8) & 0xFFFFFF) / static_cast<float>(0xFFFFFF);
}

float sampleSpectrum(const GeomSpec &spec, float position) {
    if (!spec.spectrum || spec.spectrumCount <= 0) return 0.0f;
    const float x = std::clamp(position, 0.0f, 0.9999f);
    const int index = static_cast<int>(x * spec.spectrumCount);
    return std::clamp(spec.spectrum[std::min(index, spec.spectrumCount - 1)], 0.0f, 1.0f);
}

float sampleWave(const GeomSpec &spec, float position) {
    if (!spec.wave || spec.waveCount <= 0) return 0.0f;
    const float x = std::clamp(position, 0.0f, 0.9999f);
    const int index = static_cast<int>(x * spec.waveCount);
    return spec.wave[std::min(index, spec.waveCount - 1)];
}

Color blend(const GeomSpec &spec, float t) { return palette::mix(spec.colorA, spec.colorB, t); }

void drawOrbit(const GeomSpec &spec, float cx, float cy, float radius) {
    const int orbits = std::max(1, spec.count / 8);
    for (int i = 0; i < orbits; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(orbits);
        const float angle = spec.rotation + static_cast<float>(spec.time) * (0.3f + t * 0.9f);
        const float orbitRadius = radius * (0.25f + 0.75f * t);
        const Vector2 center{cx + std::cos(angle) * orbitRadius,
                             cy + std::sin(angle) * orbitRadius * 0.7f};
        const float size = spec.thickness * (1.0f + 3.0f * sampleSpectrum(spec, t));
        const float energy = sampleSpectrum(spec, t);
        DrawCircleV(center, size + energy * spec.width * 0.01f, palette::withAlpha(blend(spec, t), spec.alpha));
    }
}

void drawSparks(const GeomSpec &spec, float cx, float cy, float radius) {
    std::vector<Particle> &particles = gParticles[spec.stateKey];
    const float energy = std::max(spec.reactivity, sampleSpectrum(spec, 0.08f));
    const int spawn = static_cast<int>(std::lround(energy * 90.0f * std::max(spec.dt, 1.0f / 240.0f) * 60.0f));
    for (int i = 0; i < std::min(spawn, 220); ++i) {
        const float angle = frand() * 6.2831853f;
        const float speed = radius * (0.4f + frand() * 2.2f);
        Particle p;
        p.position = Vector2{cx + std::cos(angle) * radius * 0.15f,
                             cy + std::sin(angle) * radius * 0.15f};
        p.velocity = Vector2{std::cos(angle) * speed, std::sin(angle) * speed};
        p.maxLife = 0.6f + frand() * 1.4f;
        p.life = p.maxLife;
        p.size = (1.5f + frand() * 3.5f) * (1.0f + spec.reactivity * 2.0f);
        p.hue = frand();
        particles.push_back(p);
    }
    for (auto &p : particles) {
        p.life -= spec.dt;
        p.position.x += p.velocity.x * spec.dt;
        p.position.y += p.velocity.y * spec.dt;
        p.velocity.x *= (1.0f - 0.6f * spec.dt);
        p.velocity.y *= (1.0f - 0.6f * spec.dt);
    }
    particles.erase(std::remove_if(particles.begin(), particles.end(),
                                   [](const Particle &p) { return p.life <= 0.0f; }),
                    particles.end());
    if (particles.size() > 4000) particles.erase(particles.begin(), particles.begin() + 1000);

    for (const auto &p : particles) {
        const float t = std::clamp(p.life / p.maxLife, 0.0f, 1.0f);
        const Color color = palette::mix(blend(spec, p.hue), Color{255, 255, 255, 255}, t * 0.5f);
        DrawCircleV(p.position, p.size * (0.4f + t), palette::withAlpha(color, spec.alpha * t));
    }
}

}  // namespace

void drawPrimitive(Shape shape, const GeomSpec &spec) {
    const float minDim = static_cast<float>(std::min(spec.width, spec.height));
    const float cx = spec.cx * static_cast<float>(spec.width);
    const float cy = spec.cy * static_cast<float>(spec.height);
    const float radius = spec.radius * minDim;
    const bool blendMode = spec.additive;

    if (blendMode) BeginBlendMode(BLEND_ADDITIVE);

    switch (shape) {
        case Shape::Circle: {
            const float r = radius * (1.0f + spec.reactivity * 0.5f);
            DrawCircleGradient(static_cast<int>(cx), static_cast<int>(cy), r, spec.colorA,
                               palette::withAlpha(spec.colorB, 0.0f));
            if (spec.thickness > 0.0f) {
                DrawCircleLinesV(Vector2{cx, cy}, r, palette::withAlpha(spec.colorB, spec.alpha));
            }
            break;
        }
        case Shape::Ring: {
            const float thickness = radius * (0.05f + 0.25f * sampleSpectrum(spec, spec.band / 64.0f));
            const float inner = std::max(1.0f, radius - thickness);
            DrawRing(Vector2{cx, cy}, inner, radius, 0.0f, 360.0f, 96,
                     palette::withAlpha(spec.colorA, spec.alpha));
            DrawRing(Vector2{cx, cy}, inner * 0.92f, inner, 0.0f, 360.0f, 96,
                     palette::withAlpha(spec.colorB, spec.alpha * 0.7f));
            break;
        }
        case Shape::RadialBars: {
            const int bars = std::max(4, spec.count);
            const float inner = radius * 0.45f;
            for (int i = 0; i < bars; ++i) {
                const float t = static_cast<float>(i) / static_cast<float>(bars);
                const float magnitude = sampleSpectrum(spec, std::pow(t, 0.8f));
                const float angle = spec.rotation + t * 6.2831853f;
                const float outer = inner + radius * (0.08f + magnitude * 1.15f);
                const Vector2 from{cx + std::cos(angle) * inner, cy + std::sin(angle) * inner};
                const Vector2 to{cx + std::cos(angle) * outer, cy + std::sin(angle) * outer};
                DrawLineEx(from, to, std::max(1.0f, spec.thickness * (0.4f + magnitude)),
                           palette::withAlpha(blend(spec, magnitude), spec.alpha));
                DrawCircleV(to, spec.thickness * 0.8f * (0.4f + magnitude),
                            palette::withAlpha(blend(spec, magnitude), spec.alpha * 0.9f));
            }
            break;
        }
        case Shape::BarSpectrum: {
            const int bars = std::max(4, spec.count);
            const float baseline = spec.height * 0.95f;
            const float width = static_cast<float>(spec.width) / static_cast<float>(bars);
            const float maxHeight = radius * 1.6f;
            for (int i = 0; i < bars; ++i) {
                const float t = static_cast<float>(i) / static_cast<float>(bars);
                const float magnitude = sampleSpectrum(spec, std::pow(t, 0.85f));
                const float h = maxHeight * (0.05f + magnitude);
                const Rectangle rect{static_cast<float>(i) * width + width * 0.12f,
                                     baseline - h, width * 0.76f, h};
                DrawRectangleGradientV(static_cast<int>(rect.x), static_cast<int>(rect.y),
                                       static_cast<int>(std::max(1.0f, rect.width)),
                                       static_cast<int>(std::max(1.0f, rect.height)),
                                       palette::withAlpha(blend(spec, magnitude), spec.alpha),
                                       palette::withAlpha(spec.colorA, spec.alpha * 0.25f));
            }
            break;
        }
        case Shape::WaveformRing: {
            const int points = 256;
            for (int i = 0; i < points; ++i) {
                const float t0 = static_cast<float>(i) / points;
                const float t1 = static_cast<float>(i + 1) / points;
                const float a0 = spec.rotation + t0 * 6.2831853f;
                const float a1 = spec.rotation + t1 * 6.2831853f;
                const float r0 = radius * (1.0f + sampleWave(spec, t0) * 0.35f);
                const float r1 = radius * (1.0f + sampleWave(spec, t1) * 0.35f);
                DrawLineEx(Vector2{cx + std::cos(a0) * r0, cy + std::sin(a0) * r0},
                           Vector2{cx + std::cos(a1) * r1, cy + std::sin(a1) * r1},
                           std::max(1.0f, spec.thickness), palette::withAlpha(blend(spec, t0), spec.alpha));
            }
            break;
        }
        case Shape::WaveformLine: {
            const int points = 320;
            const float width = static_cast<float>(spec.width);
            for (int i = 0; i < points; ++i) {
                const float t0 = static_cast<float>(i) / points;
                const float t1 = static_cast<float>(i + 1) / points;
                const float y0 = cy + sampleWave(spec, t0) * radius;
                const float y1 = cy + sampleWave(spec, t1) * radius;
                DrawLineEx(Vector2{t0 * width, y0}, Vector2{t1 * width, y1},
                           std::max(1.0f, spec.thickness), palette::withAlpha(blend(spec, t0), spec.alpha));
            }
            break;
        }
        case Shape::PolygonGrid: {
            const int cols = std::max(2, spec.count);
            const int rows = std::max(2, static_cast<int>(cols * static_cast<float>(spec.height) /
                                                          static_cast<float>(std::max(1, spec.width))));
            const float cellW = static_cast<float>(spec.width) / cols;
            const float cellH = static_cast<float>(spec.height) / rows;
            for (int y = 0; y < rows; ++y) {
                for (int x = 0; x < cols; ++x) {
                    const float t = (static_cast<float>(x) / cols + static_cast<float>(y) / rows) * 0.5f;
                    const float magnitude = sampleSpectrum(spec, t);
                    const Vector2 center{(x + 0.5f) * cellW, (y + 0.5f) * cellH};
                    float size = std::min(cellW, cellH) * (0.15f + magnitude * 0.42f);
                    size *= 1.0f + spec.reactivity * 0.4f;
                    const float angle = spec.rotation + t * 3.0f + static_cast<float>(spec.time) * 0.4f;
                    DrawPolyLinesEx(center, 6, size, angle, std::max(1.0f, spec.thickness),
                                    palette::withAlpha(blend(spec, magnitude), spec.alpha));
                }
            }
            break;
        }
        case Shape::Sparks:
            drawSparks(spec, cx, cy, radius);
            break;
        case Shape::Orbit:
            drawOrbit(spec, cx, cy, radius);
            break;
        case Shape::TextOverlay: {
            if (!spec.text.empty()) {
                const float size = spec.textSize * (minDim / 1080.0f) * (1.0f + spec.reactivity * 0.3f);
                const Vector2 measure = MeasureTextEx(GetFontDefault(), spec.text.c_str(), size, 2.0f);
                const Vector2 position{cx - measure.x * 0.5f, cy - measure.y * 0.5f};
                DrawTextEx(GetFontDefault(), spec.text.c_str(), Vector2{position.x + 2.0f, position.y + 2.0f},
                           size, 2.0f, palette::withAlpha(Color{0, 0, 0, 255}, spec.alpha * 0.55f));
                DrawTextEx(GetFontDefault(), spec.text.c_str(), position, size, 2.0f,
                           palette::withAlpha(spec.colorA, spec.alpha));
            }
            break;
        }
        default:
            break;
    }

    if (blendMode) EndBlendMode();
}

void releaseState(int stateKey) { gParticles.erase(stateKey); }
void releaseAllState() { gParticles.clear(); }

}  // namespace pf::geometry
