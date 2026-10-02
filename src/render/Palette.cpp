#include "render/Palette.h"

#include <algorithm>
#include <cmath>

#include "core/Port.h"

namespace pf::palette {

namespace {
unsigned char clamp8(float v) {
    return static_cast<unsigned char>(std::lround(std::clamp(v, 0.0f, 255.0f)));
}
}  // namespace

Color background() { return Color{18, 19, 24, 255}; }
Color backlayer() { return Color{26, 28, 35, 255}; }
Color midlayer() { return Color{33, 36, 45, 255}; }
Color frontlayer() { return Color{42, 46, 57, 255}; }
Color border() { return Color{58, 63, 78, 255}; }
Color text() { return Color{226, 229, 238, 255}; }
Color textDim() { return Color{140, 147, 166, 255}; }
Color accent() { return Color{120, 170, 255, 255}; }
Color accentAlt() { return Color{196, 140, 255, 255}; }
Color warn() { return Color{240, 190, 90, 255}; }
Color danger() { return Color{235, 100, 110, 255}; }
Color success() { return Color{120, 210, 150, 255}; }

Color mix(Color a, Color b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return Color{clamp8(a.r + (b.r - a.r) * t), clamp8(a.g + (b.g - a.g) * t),
                 clamp8(a.b + (b.b - a.b) * t), clamp8(a.a + (b.a - a.a) * t)};
}

Color withAlpha(Color c, float alpha) {
    c.a = clamp8(alpha * 255.0f);
    return c;
}

Color modulate(Color c, float factor) {
    c.r = clamp8(c.r * factor);
    c.g = clamp8(c.g * factor);
    c.b = clamp8(c.b * factor);
    return c;
}

Color fromHex(unsigned int rgb, float alpha) {
    return Color{static_cast<unsigned char>((rgb >> 16) & 0xFF),
                 static_cast<unsigned char>((rgb >> 8) & 0xFF),
                 static_cast<unsigned char>(rgb & 0xFF), clamp8(alpha * 255.0f)};
}

Color hue(float h, float s, float v, float a) { return colorFromHsv(h, s, v, a); }

unsigned int toHex(Color c) {
    return (static_cast<unsigned int>(c.r) << 16) | (static_cast<unsigned int>(c.g) << 8) |
           static_cast<unsigned int>(c.b);
}

Color fromPacked(unsigned long long packed) {
    return Color{static_cast<unsigned char>((packed >> 24) & 0xFF),
                 static_cast<unsigned char>((packed >> 16) & 0xFF),
                 static_cast<unsigned char>((packed >> 8) & 0xFF),
                 static_cast<unsigned char>(packed & 0xFF)};
}

unsigned long long toPacked(Color c) {
    return (static_cast<unsigned long long>(c.r) << 24) |
           (static_cast<unsigned long long>(c.g) << 16) |
           (static_cast<unsigned long long>(c.b) << 8) |
           static_cast<unsigned long long>(c.a);
}

}  // namespace pf::palette
