// Executes the pipeline: allocates render targets, uploads analysis textures and
// runs every block for one frame.
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "core/Graph.h"
#include "render/ShaderLibrary.h"

namespace pf {

struct RenderStats {
    double frameMs = 0.0;
    int nodesEvaluated = 0;
    int shaderPasses = 0;
    int pooledTargets = 0;
    int peakTargets = 0;
};

// Vector/matrix values handed to a shader pass (uVector2/3/4, uMatrix).
struct ShaderVectorUniforms {
    Vector2 vec2{};
    Vector3 vec3{};
    Vector4 vec4{};
    Matrix matrix{};
    bool useVec2 = false;
    bool useVec3 = false;
    bool useVec4 = false;
    bool useMatrix = false;
};

class Renderer {
public:
    bool init(std::string *error);
    void shutdown();

    // ---- render targets -------------------------------------------------
    ImageBufferPtr acquire(int width, int height);
    void recycleAll();

    // Previous-frame buffer for feedback effects, keyed by block id.
    ImageBufferPtr persistent(int nodeId, int width, int height);
    void resetPersistent();

    void beginTarget(const ImageBufferPtr &target, bool clear, Color clearColor);
    void endTarget();
    int currentWidth() const { return currentWidth_; }
    int currentHeight() const { return currentHeight_; }
    bool insideTarget() const { return current_ != nullptr; }

    // ---- drawing --------------------------------------------------------
    void blit(const ImageBufferPtr &source, float alpha = 1.0f);
    void drawShaderPass(Shader *shader, const EvalContext &ctx, Texture2D prev, Texture2D input2,
                        Texture2D feedback, const float *user, int userCount, Color colorA,
                        Color colorB, const ShaderVectorUniforms &vectors = ShaderVectorUniforms{});

    Texture2D whiteTexture() const { return white_; }
    Texture2D blackTexture() const { return black_; }
    Texture2D spectrumTexture() const { return spectrum_; }
    Texture2D waveformTexture() const { return waveform_; }
    void updateAnalysisTextures(const EvalContext &ctx);
    // Uploads one analysis's spectrum row and waveform window to the shader
    // textures. Spectrum blocks call this for their own Analysis input so a
    // processed stream does not show the imported clip's textures.
    void uploadAnalysisTextures(const AnalysisData &analysis, double time);

    // ---- frame ----------------------------------------------------------
    ImageBufferPtr renderFrame(Graph &graph, EvalContext &ctx, std::string *error);
    ImageBufferPtr outputTarget() const { return output_; }

    ShaderLibrary &shaders() { return shaders_; }
    const RenderStats &stats() const { return stats_; }

private:
    struct PooledTarget {
        ImageBufferPtr buffer;
        bool inUse = false;
    };

    struct UniformLocs {
        int time = -1;
        int frame = -1;
        int duration = -1;
        int progress = -1;
        int resolution = -1;
        int texel = -1;
        int bass = -1;
        int mid = -1;
        int treble = -1;
        int level = -1;
        int onset = -1;
        int beat = -1;
        int user = -1;
        int colorA = -1;
        int colorB = -1;
        int vec2 = -1;
        int vec3 = -1;
        int vec4 = -1;
        int matrix = -1;
        int prev = -1;
        int input2 = -1;
        int feedback = -1;
        int spectrum = -1;
        int waveform = -1;
    };

    const UniformLocs &locations(Shader *shader);
    ImageBufferPtr allocate(int width, int height);
    void updateBeatPhase(const EvalContext &ctx);

    std::vector<PooledTarget> pool_;
    std::unordered_map<int, ImageBufferPtr> persistent_;
    std::unordered_map<unsigned int, UniformLocs> locationCache_;
    ShaderLibrary shaders_;
    Texture2D white_{};
    Texture2D black_{};
    Texture2D spectrum_{};
    Texture2D waveform_{};
    std::vector<unsigned char> spectrumPixels_;
    std::vector<unsigned char> waveformPixels_;
    std::vector<float> scratchSpectrum_;
    std::vector<float> scratchWave_;
    ImageBufferPtr current_;
    ImageBufferPtr output_;
    int currentWidth_ = 0;
    int currentHeight_ = 0;
    RenderStats stats_;
    int liveTargets_ = 0;
    int peakTargets_ = 0;
    // beat tracking state
    double lastOnsetTime_ = -1.0;
    double beatInterval_ = 0.5;
    double lastOnsetLevel_ = 0.0;
};

// Convenience: binds a target, clears it and prepares the normalised projection.
struct RenderTargetScope {
    Renderer *renderer = nullptr;
    explicit RenderTargetScope(Renderer *r, const ImageBufferPtr &target, Color clearColor)
        : renderer(r) {
        renderer->beginTarget(target, true, clearColor);
    }
    ~RenderTargetScope() { renderer->endTarget(); }
};

}  // namespace pf
