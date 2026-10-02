#version 330
// A shader that declares #version itself is used verbatim, so it must declare
// everything it needs. Use this form when porting existing GLSL.

in vec2 fragTexCoord;
in vec4 fragColor;
out vec4 finalColor;

uniform mat4 mvp;
uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform float uTime;
uniform vec2 uResolution;

void main() {
    vec2 uv = fragTexCoord;
    vec3 col = vec3(0.5 + 0.5 * sin(uTime + uv.x * 6.2831853),
                    0.5 + 0.5 * sin(uTime * 1.3 + uv.y * 6.2831853),
                    0.6);
    float grid = step(0.99, fract(uv.x * 32.0)) + step(0.99, fract(uv.y * 18.0));
    col += grid * 0.25;
    finalColor = vec4(col, 1.0);
}
