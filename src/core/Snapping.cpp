#include "core/Snapping.h"

#include <algorithm>
#include <cmath>

namespace pf {

namespace {

struct AxisSnap {
    bool found = false;
    float correction = 0.0f;
    int priority = 0;
};

void consider(AxisSnap &best, float correction, int priority, float threshold) {
    const float distance = std::fabs(correction);
    if (distance > threshold) return;
    const float bestDistance = std::fabs(best.correction);
    if (!best.found || distance < bestDistance - 1.0e-4f ||
        (std::fabs(distance - bestDistance) <= 1.0e-4f && priority < best.priority)) {
        best.found = true;
        best.correction = correction;
        best.priority = priority;
    }
}

}  // namespace

SnapResult snapBlock(SnapRect moving, const std::vector<SnapRect> &targets,
                     float threshold, float gap, float grid, bool useGrid) {
    AxisSnap xSnap;
    AxisSnap ySnap;
    for (const SnapRect &target : targets) {
        const float movedX[] = {
            moving.x, moving.x + moving.width * 0.5f, moving.x + moving.width};
        const float movedY[] = {
            moving.y, moving.y + moving.height * 0.5f, moving.y + moving.height};
        const float fixedX[] = {
            target.x, target.x + target.width * 0.5f, target.x + target.width};
        const float fixedY[] = {
            target.y, target.y + target.height * 0.5f, target.y + target.height};
        for (const float fixed : fixedX) {
            for (const float moved : movedX) {
                consider(xSnap, fixed - moved, 1, threshold);
            }
        }
        for (const float fixed : fixedY) {
            for (const float moved : movedY) {
                consider(ySnap, fixed - moved, 1, threshold);
            }
        }

        const bool nearVertically =
            moving.y < target.y + target.height + threshold &&
            moving.y + moving.height > target.y - threshold;
        if (nearVertically) {
            consider(xSnap, target.x + target.width + gap - moving.x, 0, threshold);
            consider(xSnap, target.x - gap - (moving.x + moving.width), 0, threshold);
        }
        const bool nearHorizontally =
            moving.x < target.x + target.width + threshold &&
            moving.x + moving.width > target.x - threshold;
        if (nearHorizontally) {
            consider(ySnap, target.y + target.height + gap - moving.y, 0, threshold);
            consider(ySnap, target.y - gap - (moving.y + moving.height), 0, threshold);
        }
    }

    SnapResult result{moving.x, moving.y, false, false};
    const bool blockSnapped = xSnap.found || ySnap.found;
    if (xSnap.found) {
        result.x += xSnap.correction;
        result.alignedX = true;
    } else if (!blockSnapped && useGrid && grid > 0.0f) {
        result.x = std::round(result.x / grid) * grid;
    }
    if (ySnap.found) {
        result.y += ySnap.correction;
        result.alignedY = true;
    } else if (!blockSnapped && useGrid && grid > 0.0f) {
        result.y = std::round(result.y / grid) * grid;
    }
    return result;
}

}  // namespace pf
