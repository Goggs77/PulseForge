// Caustic water on top of the upstream image.

void main() {
    vec2 uv = pfUv();
    vec2 p = uv * vec2(uResolution.x / uResolution.y, 1.0) * 3.2;

    float t = uTime * (0.35 + uMid);
    float n = pfFbm(p + vec2(t, -t * 0.7));
    float n2 = pfFbm(p * 1.7 - vec2(t * 0.9, t));
    float caustic = pow(1.0 - abs(n - n2) * 2.2, 3.0);

    vec3 base = pfPrev(uv).rgb;
    vec3 tint = mix(uColorA.rgb, uColorB.rgb, n);
    vec3 col = base + tint * caustic * (0.6 + 1.4 * uLevel);
    col += caustic * uOnset * 0.5;
    finalColor = vec4(col, 1.0);
}
