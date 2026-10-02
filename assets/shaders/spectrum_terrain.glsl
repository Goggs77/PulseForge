// Spectrum terrain: a wireframe landscape whose height comes from the spectrum.

void main() {
    vec2 uv = pfUv();
    float horizon = 0.42 + 0.06 * uUser[3];

    vec3 col = vec3(0.02, 0.03, 0.06);

    // sky with a slow gradient
    col += mix(uColorA.rgb, uColorB.rgb, uv.y / max(horizon, 0.001)) * 0.10;

    // ground: sample the spectrum along the x axis, scrolling with time
    if (uv.y > horizon) {
        float depth = (uv.y - horizon) / (1.0 - horizon);
        float scale = 1.0 / max(depth, 0.02);
        float x = (uv.x - 0.5) * scale * 0.6 + uTime * 0.05;
        float xf = fract(abs(x));
        float band = pfSpectrum(xf);
        float ridge = horizon + (1.0 - depth) * (0.02 + band * 0.18);

        float line = smoothstep(0.004, 0.0, abs(uv.y - ridge));
        col += line * mix(uColorA.rgb, uColorB.rgb, band) * (1.4 + 2.0 * band);

        // vertical grid lines receding into the distance
        float grid = smoothstep(0.02 * depth + 0.002, 0.0, abs(xf - 0.5));
        col += grid * vec3(0.10, 0.16, 0.24) * (1.0 - depth);
    }

    col += uOnset * 0.15;
    finalColor = vec4(col, 1.0);
}
