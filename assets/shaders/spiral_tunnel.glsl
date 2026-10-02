// Spiral tunnel.
//
// Files that do not start with #version get the PulseForge preamble prepended,
// so everything declared there is available: uv in fragTexCoord, pfPrev(),
// pfSpectrum(), pfWave(), uTime, uBass/uMid/uTreble, uUser[0..7], uColorA/B.
// Declare your own uniforms only if they are not already in the preamble.

void main() {
    vec2 uv = pfUv();
    vec2 p = uv - 0.5;
    p.x *= uResolution.x / uResolution.y;

    float radius = length(p);
    float angle = atan(p.y, p.x);
    float depth = 0.30 / max(radius, 0.0015);

    float amount = uUser[0];          // patch a scalar port in the editor
    angle += depth * 0.35 + uTime * 0.55 + amount * 2.0;
    angle += sin(depth * 0.6 + uTime * 1.3) * (0.25 + uBass * 0.9);

    float u = angle / 3.14159265;
    float v = fract(depth * 0.22 - uTime * 0.42);

    float bands = pfSpectrum(clamp(v, 0.0, 1.0));
    vec3 col = mix(uColorA.rgb, uColorB.rgb, bands);
    // Keep a floor so the tunnel reads even when the spectrum is quiet.
    col *= 0.55 + 1.6 * bands + 1.2 * uLevel;
    col += uColorA.rgb * 0.12 * (1.0 - bands);

    // Glowing core that pulses with the bass.
    col += uColorB.rgb * (0.25 + uBass) * exp(-radius * 9.0) * 1.6;
    col *= smoothstep(0.0, 0.42, radius);

    // A touch of the previous frame keeps the tunnel from looking sterile.
    col = mix(col, pfFeedback(vec2(u * 0.5 + 0.5, v)).rgb, 0.25 + 0.25 * uUser[1]);
    finalColor = vec4(col, 1.0);
}
