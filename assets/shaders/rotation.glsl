// Rotation template - spins the incoming Image about its centre.
//
// uUser[0] is the angle in turns (1 = a full clockwise turn) and is also a
// Scalar input on the Shader block, so a slow LFO turns it into a spinning
// plate. The aspect correction keeps the rotation round on 16:9 frames.

void main() {
    float turns = uUser[0];
    float aspect = uResolution.x/uResolution.y;
    vec2 p = (pfUv() - 0.5)*vec2(aspect, 1.0);
    // Inverse map: to turn the image by +turns, sample the source turned back.
    vec2 source = pfRotate(p, -turns*6.2831853);
    source.x /= aspect;
    source += 0.5;
    vec2 edge = min(source, 1.0 - source)/max(uTexel, vec2(1.0e-6));
    float inside = smoothstep(0.0, 2.0, min(edge.x, edge.y));
    finalColor = vec4(pfPrev(source).rgb*inside, 1.0);
}
