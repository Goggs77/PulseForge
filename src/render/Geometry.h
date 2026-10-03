// Geometric elements drawn into the current render target.
#pragma once

#include <string>
#include <vector>

#include "raylib.h"

namespace pf::geometry {

enum class Shape : int {
    None = 0,
    Circle,
    Ring,
    PolygonGrid,
    Sparks,
    Orbit,
    TextOverlay,
    Count,
};

const char *shapeName(Shape shape);
std::vector<std::string> shapeNames();

struct GeomSpec {
    int width = 1920;
    int height = 1080;

    float cx = 0.5f;   // normalised 0..1
    float cy = 0.5f;
    float radius = 0.25f;    // normalised against the smaller dimension
    float thickness = 4.0f;
    float rotation = 0.0f;   // radians
    int count = 64;

    Color colorA = WHITE;
    Color colorB = WHITE;
    float alpha = 1.0f;
    bool additive = true;

    std::string text;
    float textSize = 72.0f;

    // Animation state for the particle shapes (persisted by the caller).
    int stateKey = 0;
    float dt = 1.0f / 60.0f;
    double time = 0.0;
};

void drawPrimitive(Shape shape, const GeomSpec &spec);
void releaseState(int stateKey);
void releaseAllState();

}  // namespace pf::geometry
