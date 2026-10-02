// Shared colour palette: keeps the editor chrome and the on-canvas widgets
// visually consistent with the CrystalGUI crystalline dark theme.
#pragma once

#include "raylib.h"

namespace pf::palette {

Color background();
Color backlayer();
Color midlayer();
Color frontlayer();
Color border();
Color text();
Color textDim();
Color accent();
Color accentAlt();
Color warn();
Color danger();
Color success();

Color mix(Color a, Color b, float t);
Color withAlpha(Color c, float alpha);
Color modulate(Color c, float factor);
Color fromHex(unsigned int rgb, float alpha = 1.0f);
Color hue(float h, float s, float v, float a = 1.0f);
// Packed 0xffRRGGBB helpers used by the project files.
unsigned int toHex(Color c);
Color fromPacked(unsigned long long packed);
unsigned long long toPacked(Color c);

}  // namespace pf::palette
