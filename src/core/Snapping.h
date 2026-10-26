// Geometry-only snapping used by the graph canvas.
#pragma once

#include <vector>

namespace pf {

struct SnapRect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

struct SnapResult {
    float x = 0.0f;
    float y = 0.0f;
    bool alignedX = false;
    bool alignedY = false;
};

// Aligns edges or centres first, then tries adjacent edges separated by `gap`.
// Any block-derived snap suppresses the grid for the whole result, preventing
// mixed-axis jumps such as nearby horizontally plus grid vertically.
SnapResult snapBlock(SnapRect moving, const std::vector<SnapRect> &targets,
                     float threshold, float gap, float grid, bool useGrid);

}  // namespace pf
