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

// Draws the live content of `node` inside `body` (screen space). `state` supplies
// the analysis and audio the previews read from.
void drawNodeVisual(UiState &state, const Node &node, Rectangle body, float zoom);

}  // namespace pf
