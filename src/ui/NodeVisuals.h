// Live content drawn inside a block's body on the graph canvas: spectra,
// level bars, value/time diagrams and waveforms.
#pragma once

#include "raylib.h"

struct UiState;

namespace pf {

class Node;

// Extra body height (in world units, before the canvas zoom) the block wants for
// its live content. Blocks without content return 0.
float nodeVisualHeight(const Node &node);

// Sorting blocks (Sticky Note, Group) draw as greyscale cards instead of a
// category colour: a light card with deep grey ink on the dark theme, and the
// other way round on the light theme. `alphaScale` fades a disabled block.
struct SortingColours {
    Color ink{};
    Color header{};
    Color body{};
    Color border{};
};
SortingColours sortingColours(float alphaScale = 1.0f);

// Draws the live content of `node` inside `body` (screen space), clipped to the
// intersection of `body` and `clip` (usually the canvas viewport) so a block that
// hangs over the edge of the pipeline panel never paints outside it. `state`
// supplies the analysis and audio the previews read from.
void drawNodeVisual(UiState &state, const Node &node, Rectangle body, Rectangle clip, float zoom);

// Some blocks put an interactive pivot in their live content (the Signal Filter's
// cutoff/resonance handle). The canvas asks these instead of dragging the block.
bool nodeVisualHasPivot(const Node &node);
void nodeVisualPivotDrag(UiState &state, Node &node, Rectangle body, Vector2 mouse);

}  // namespace pf
