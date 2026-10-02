// PulseForge diagnostic probe: finds a rendering mode in which a render target
// uses the normal image convention (visual top at texture coordinate v = 0),
// so that shader sampling, pass chaining, screen display and glReadPixels all
// agree and video frames need no flip.
//
// Run:  pf_probe

#include <cstdio>
#include <cstring>
#include <vector>

#include "raylib.h"
#include "rlgl.h"
#include "glad.h"

static char Classify(unsigned char r, unsigned char g, unsigned char b) {
    const bool R = r > 128, G = g > 128, B = b > 128;
    if (R && G && B) return 'W';
    if (R && G) return 'Y';
    if (R && B) return 'M';
    if (G && B) return 'C';
    if (R) return 'R';
    if (G) return 'G';
    if (B) return 'B';
    if (r > 20 || g > 20 || b > 20) return '?';
    return '.';
}

static void PrintMap(const char *label, const unsigned char *buf, int w, int h) {
    std::printf("%s\n", label);
    for (int gy = 0; gy < 8; ++gy) {
        std::printf("   ");
        for (int gx = 0; gx < 8; ++gx) {
            const int x = gx * (w / 8) + w / 16;
            const int y = gy * (h / 8) + h / 16;
            const unsigned char *p = buf + (static_cast<size_t>(y) * w + x) * 4;
            std::printf("%c", Classify(p[0], p[1], p[2]));
        }
        std::printf("\n");
    }
}

static void ReadTarget(RenderTexture2D rt, std::vector<unsigned char> &out) {
    out.assign(static_cast<size_t>(rt.texture.width) * rt.texture.height * 4, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, rt.id);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, rt.texture.width, rt.texture.height, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// Quadrants drawn in raylib screen coordinates (y grows downwards).
static void DrawQuadrants() {
    ClearBackground(BLACK);
    DrawRectangle(0, 0, 32, 32, RED);       // visual top-left
    DrawRectangle(32, 0, 32, 32, GREEN);    // visual top-right
    DrawRectangle(0, 32, 32, 32, WHITE);    // visual bottom-left
    DrawRectangle(32, 32, 32, 32, BLUE);    // visual bottom-right
    DrawRectangle(0, 0, 64, 4, YELLOW);     // strip on the visual top edge
}

static void SetMatrixMode(int mode, RenderTexture2D rt) {
    rlDrawRenderBatchActive();
    switch (mode) {
        case 0:
            break;
        case 1:  // re-apply raylib's own render-target ortho
            rlMatrixMode(RL_PROJECTION);
            rlLoadIdentity();
            rlOrtho(0, rt.texture.width, rt.texture.height, 0, 0.0f, 1.0f);
            rlMatrixMode(RL_MODELVIEW);
            rlLoadIdentity();
            break;
        case 2:  // inverted ortho: visual top at v = 0
            rlMatrixMode(RL_PROJECTION);
            rlLoadIdentity();
            rlOrtho(0, rt.texture.width, 0, rt.texture.height, 0.0f, 1.0f);
            rlMatrixMode(RL_MODELVIEW);
            rlLoadIdentity();
            break;
        case 3:  // modelview flip: scale then translate
            rlMatrixMode(RL_MODELVIEW);
            rlLoadIdentity();
            rlScalef(1.0f, -1.0f, 1.0f);
            rlTranslatef(0.0f, -(float)rt.texture.height, 0.0f);
            break;
        case 4:  // modelview flip: translate then scale
            rlMatrixMode(RL_MODELVIEW);
            rlLoadIdentity();
            rlTranslatef(0.0f, (float)rt.texture.height, 0.0f);
            rlScalef(1.0f, -1.0f, 1.0f);
            break;
        default:
            break;
    }
}

static void Variant(int mode, const char *label, std::vector<unsigned char> &pixels) {
    const int W = 64, H = 64;
    RenderTexture2D rt = LoadRenderTexture(W, H);
    BeginTextureMode(rt);
    rlDisableBackfaceCulling();
    SetMatrixMode(mode, rt);
    DrawQuadrants();
    EndTextureMode();
    ReadTarget(rt, pixels);
    PrintMap(label, pixels.data(), W, H);
    UnloadRenderTexture(rt);
}

static const char *kVertexShader = R"(#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec4 vertexColor;
uniform mat4 mvp;
out vec2 fragTexCoord;
out vec4 fragColor;
void main() {
    fragTexCoord = vertexTexCoord;
    fragColor = vertexColor;
    gl_Position = mvp*vec4(vertexPosition, 1.0);
}
)";

static const char *kFragmentShader = R"(#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform vec4 colDiffuse;
out vec4 finalColor;
void main() {
    finalColor = vec4(texture(texture0, fragTexCoord).rgb, 1.0);
}
)";

// Verifies shader pass chaining in the normalised mode: a passthrough must not
// change orientation, geometry drawn after a pass must still land correctly,
// and the PBO readback must agree.
static void ChainTest(std::vector<unsigned char> &pixels) {
    const int W = 64, H = 64;

    RenderTexture2D src = LoadRenderTexture(W, H);
    BeginTextureMode(src);
    rlDisableBackfaceCulling();
    SetMatrixMode(2, src);
    DrawQuadrants();
    EndTextureMode();

    Shader shader = LoadShaderFromMemory(kVertexShader, kFragmentShader);
    if (!IsShaderValid(shader)) {
        std::printf("[5] FAIL: shader did not compile\n");
        UnloadRenderTexture(src);
        return;
    }

    RenderTexture2D dst = LoadRenderTexture(W, H);
    BeginTextureMode(dst);
    rlDisableBackfaceCulling();
    SetMatrixMode(2, dst);
    ClearBackground(BLACK);
    BeginShaderMode(shader);
    DrawTexturePro(src.texture, Rectangle{0, 0, (float)W, (float)H},
                   Rectangle{0, 0, (float)W, (float)H}, Vector2{0, 0}, 0.0f, WHITE);
    EndShaderMode();
    EndTextureMode();
    ReadTarget(dst, pixels);
    PrintMap("[5] shader passthrough in normalised mode (must match [2])", pixels.data(), W, H);

    RenderTexture2D geo = LoadRenderTexture(W, H);
    BeginTextureMode(geo);
    rlDisableBackfaceCulling();
    SetMatrixMode(2, geo);
    ClearBackground(BLACK);
    BeginShaderMode(shader);
    DrawTexturePro(dst.texture, Rectangle{0, 0, (float)W, (float)H},
                   Rectangle{0, 0, (float)W, (float)H}, Vector2{0, 0}, 0.0f, WHITE);
    EndShaderMode();
    DrawRectangle(0, 0, 64, 8, YELLOW);   // geometry after the pass, visual top
    EndTextureMode();
    ReadTarget(geo, pixels);
    PrintMap("[6] shader pass then geometry on top edge (row 0 must be yellow)", pixels.data(), W, H);

    GLuint pbo = 0;
    glGenBuffers(1, &pbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
    glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)W * H * 4, nullptr, GL_STREAM_READ);
    glBindFramebuffer(GL_FRAMEBUFFER, geo.id);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    void *mapped = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)W * H * 4, GL_MAP_READ_BIT);
    if (mapped) {
        std::vector<unsigned char> pboPixels(static_cast<size_t>(W) * H * 4);
        std::memcpy(pboPixels.data(), mapped, pboPixels.size());
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        PrintMap("[7] PBO readback (must match [6])", pboPixels.data(), W, H);
    } else {
        std::printf("[7] PBO readback FAILED\n");
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glDeleteBuffers(1, &pbo);

    UnloadRenderTexture(geo);
    UnloadRenderTexture(dst);
    UnloadShader(shader);
    UnloadRenderTexture(src);
}

int main() {
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(512, 512, "pf_probe");
    if (!IsWindowReady()) {
        std::printf("FAIL: window could not be created\n");
        return 1;
    }
    std::printf("GL : %s | %s\n", (const char *)glGetString(GL_RENDERER),
                (const char *)glGetString(GL_VERSION));
    std::printf("Goal: map row 0 = YYYYYYYY (yellow top strip), then RRRRGGGG x3, WWWWBBBB x4.\n\n");

    std::vector<unsigned char> pixels;
    Variant(0, "[0] raylib default (no override) -- expected to be flipped", pixels);
    Variant(1, "[1] re-applied raylib ortho -- sanity check", pixels);
    Variant(2, "[2] inverted ortho rlOrtho(0,w,0,h)", pixels);
    Variant(3, "[3] modelview scale(-1) then translate", pixels);
    Variant(4, "[4] modelview translate then scale(-1)", pixels);

    ChainTest(pixels);

    CloseWindow();
    std::printf("\nprobe complete\n");
    return 0;
}
