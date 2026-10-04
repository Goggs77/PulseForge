// Scale template - zooms the incoming Image about its centre.
//
// uUser[0] is the zoom amount (1 = unchanged, >1 zooms in, <1 zooms out) and
// is also a Scalar input on the Shader block, so an LFO or Automation can
// drive it. Copy the file, rename it and edit from here.

void main() {
    float amount = pfParam(uUser[0], 1.0);
    // Inverse map: which source pixel ends up at this destination pixel.
    vec2 source = (pfUv() - 0.5)/amount + 0.5;
    // Outside the source image fade over two texels instead of smearing the
    // clamped edge pixels.
    vec2 edge = min(source, 1.0 - source)/max(uTexel, vec2(1.0e-6));
    float inside = smoothstep(0.0, 2.0, min(edge.x, edge.y));
    finalColor = vec4(pfPrev(source).rgb*inside, 1.0);
}
