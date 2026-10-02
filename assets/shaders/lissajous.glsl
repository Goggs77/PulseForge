// Lissajous figures: two oscillators driven by band energies.

void main() {
    vec2 uv = (pfUv() - 0.5) * vec2(uResolution.x / uResolution.y, 1.0);
    vec3 col = vec3(0.0);

    float a = 2.0 + floor(uUser[0] * 6.0);
    float b = 3.0 + floor(uUser[1] * 6.0);
    float phase = uTime * mix(0.4, 2.2, uTreble) + uUser[2] * 6.2831853;
    float scale = 0.35 + 0.10 * uBass;

    for (int i = 0; i < 3; ++i) {
        float fi = float(i);
        vec2 point = vec2(sin(a * uTime * 0.7 + phase + fi * 0.6),
                          cos(b * uTime * 0.9 - phase + fi * 0.6)) * scale;
        float d = length(uv - point);
        col += mix(uColorA.rgb, uColorB.rgb, fi / 2.0) * exp(-d * 26.0) * (0.8 + uLevel);
    }

    col += 0.05 * pfFeedback(pfUv()).rgb;
    finalColor = vec4(col, 1.0);
}
