#include "render/Renderer.h"

#include <algorithm>
#include <cmath>

#include "core/Registry.h"
#include "rlgl.h"

namespace pf {

// ---------------------------------------------------------------------------
// Uniform helpers.
//
// NOTE: raylib's GetShaderLocation() returns the raw OpenGL uniform location
// (see rlGetLocationUniform), NOT an index into Shader::locs. Feed it straight
// to rlSetUniform/rlSetUniformSampler - indexing locs[] with it silently writes
// to unrelated uniforms.
// ---------------------------------------------------------------------------

static inline void setFloat(int location, float value) {
    if (location >= 0) rlSetUniform(location, &value, RL_SHADER_UNIFORM_FLOAT, 1);
}

static inline void setVec2(int location, float x, float y) {
    if (location < 0) return;
    const float values[2] = {x, y};
    rlSetUniform(location, values, RL_SHADER_UNIFORM_VEC2, 1);
}

static inline void setVec4(int location, Color color) {
    if (location < 0) return;
    const float values[4] = {color.r / 255.0f, color.g / 255.0f, color.b / 255.0f,
                             color.a / 255.0f};
    rlSetUniform(location, values, RL_SHADER_UNIFORM_VEC4, 1);
}

static inline void setFloatArray(int location, const float *values, int count) {
    if (location < 0 || count <= 0) return;
    rlSetUniform(location, values, RL_SHADER_UNIFORM_FLOAT, count);
}

static inline void setVec3(int location, Vector3 value) {
    if (location < 0) return;
    const float values[3] = {value.x, value.y, value.z};
    rlSetUniform(location, values, RL_SHADER_UNIFORM_VEC3, 1);
}

static inline void setVec4f(int location, Vector4 value) {
    if (location < 0) return;
    const float values[4] = {value.x, value.y, value.z, value.w};
    rlSetUniform(location, values, RL_SHADER_UNIFORM_VEC4, 1);
}

static inline void setMatrix(int location, const Matrix &value) {
    if (location < 0) return;
    rlSetUniformMatrix(location, value);
}

static inline void setSampler(int location, unsigned int textureId) {
    if (location < 0 || textureId == 0) return;
    rlSetUniformSampler(location, textureId);
}

// ---------------------------------------------------------------------------
// Initialisation
// ---------------------------------------------------------------------------

bool Renderer::init(std::string *error) {
    Image whiteImage{};
    static unsigned char whitePixel[4] = {255, 255, 255, 255};
    whiteImage.data = whitePixel;
    whiteImage.width = 1;
    whiteImage.height = 1;
    whiteImage.mipmaps = 1;
    whiteImage.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    white_ = LoadTextureFromImage(whiteImage);

    static unsigned char blackPixel[4] = {0, 0, 0, 255};
    Image blackImage = whiteImage;
    blackImage.data = blackPixel;
    black_ = LoadTextureFromImage(blackImage);

    spectrumPixels_.assign(256u * 4u, 0);
    waveformPixels_.assign(512u * 4u, 0);
    scratchSpectrum_.assign(256, 0.0f);
    scratchWave_.assign(512, 0.0f);

    Image spectrumImage = whiteImage;
    spectrumImage.data = spectrumPixels_.data();
    spectrumImage.width = 256;
    spectrumImage.height = 1;
    spectrum_ = LoadTextureFromImage(spectrumImage);
    SetTextureFilter(spectrum_, TEXTURE_FILTER_BILINEAR);

    Image waveImage = whiteImage;
    waveImage.data = waveformPixels_.data();
    waveImage.width = 512;
    waveImage.height = 1;
    waveform_ = LoadTextureFromImage(waveImage);
    SetTextureFilter(waveform_, TEXTURE_FILTER_BILINEAR);

    if (white_.id == 0 || black_.id == 0 || spectrum_.id == 0 || waveform_.id == 0) {
        if (error) *error = "renderer: could not create helper textures";
        return false;
    }
    return true;
}

void Renderer::shutdown() {
    for (auto &entry : pool_) {
        if (entry.buffer && entry.buffer->valid()) UnloadRenderTexture(entry.buffer->texture);
    }
    pool_.clear();
    for (auto &kv : persistent_) {
        if (kv.second && kv.second->valid()) UnloadRenderTexture(kv.second->texture);
    }
    persistent_.clear();
    if (output_ && output_->valid()) UnloadRenderTexture(output_->texture);
    output_.reset();
    current_.reset();
    shaders_.reset();
    if (white_.id) UnloadTexture(white_);
    if (black_.id) UnloadTexture(black_);
    if (spectrum_.id) UnloadTexture(spectrum_);
    if (waveform_.id) UnloadTexture(waveform_);
    white_ = black_ = spectrum_ = waveform_ = Texture2D{};
    locationCache_.clear();
}

// ---------------------------------------------------------------------------
// Targets
// ---------------------------------------------------------------------------

ImageBufferPtr Renderer::allocate(int width, int height) {
    width = std::max(1, width);
    height = std::max(1, height);
    auto buffer = std::make_shared<ImageBuffer>();
    buffer->texture = LoadRenderTexture(width, height);
    buffer->width = width;
    buffer->height = height;
    if (!buffer->valid()) return nullptr;
    SetTextureFilter(buffer->texture.texture, TEXTURE_FILTER_BILINEAR);
    return buffer;
}

ImageBufferPtr Renderer::acquire(int width, int height) {
    width = std::max(1, width);
    height = std::max(1, height);
    for (auto &entry : pool_) {
        if (!entry.inUse && entry.buffer && entry.buffer->width == width &&
            entry.buffer->height == height) {
            entry.inUse = true;
            entry.tick = ++poolTick_;
            ++liveTargets_;
            peakTargets_ = std::max(peakTargets_, liveTargets_);
            return entry.buffer;
        }
    }
    ImageBufferPtr created = allocate(width, height);
    if (!created) return nullptr;
    // Bound the pool: a block that modulates its resolution can otherwise keep
    // every visited size alive. Evict the least-recently-used free target.
    constexpr size_t kMaxPooledTargets = 12;
    if (pool_.size() >= kMaxPooledTargets) {
        size_t victim = pool_.size();
        unsigned long long oldest = ~0ull;
        for (size_t i = 0; i < pool_.size(); ++i) {
            if (pool_[i].inUse || !pool_[i].buffer) continue;
            if (pool_[i].tick < oldest) {
                oldest = pool_[i].tick;
                victim = i;
            }
        }
        if (victim < pool_.size()) {
            if (pool_[victim].buffer->valid()) {
                UnloadRenderTexture(pool_[victim].buffer->texture);
            }
            pool_.erase(pool_.begin() + static_cast<long>(victim));
        }
    }
    pool_.push_back(PooledTarget{created, true, ++poolTick_});
    ++liveTargets_;
    peakTargets_ = std::max(peakTargets_, liveTargets_);
    stats_.pooledTargets = static_cast<int>(pool_.size());
    stats_.peakTargets = peakTargets_;
    return created;
}

void Renderer::recycleAll() {
    for (auto &entry : pool_) entry.inUse = false;
    liveTargets_ = 0;
}

ImageBufferPtr Renderer::persistent(int nodeId, int width, int height) {
    auto it = persistent_.find(nodeId);
    if (it != persistent_.end() && it->second && it->second->width == width &&
        it->second->height == height) {
        return it->second;
    }
    if (it != persistent_.end() && it->second && it->second->valid()) {
        UnloadRenderTexture(it->second->texture);
    }
    ImageBufferPtr created = allocate(width, height);
    if (!created) return nullptr;
    // Clear it so the first frame has a defined starting point.
    BeginTextureMode(created->texture);
    rlDisableBackfaceCulling();
    ClearBackground(BLANK);
    EndTextureMode();
    persistent_[nodeId] = created;
    return created;
}

void Renderer::resetPersistent() {
    for (auto &kv : persistent_) {
        if (kv.second && kv.second->valid()) UnloadRenderTexture(kv.second->texture);
    }
    persistent_.clear();
}

void Renderer::beginTarget(const ImageBufferPtr &target, bool clear, Color clearColor) {
    if (!target || !target->valid()) return;
    BeginTextureMode(target->texture);
    // Verified experimentally: raylib's default render-target projection puts
    // the visual top at v = 1 and has backface culling enabled, both of which
    // break shader chaining. See docs/RENDERING_NOTES.md.
    rlDisableBackfaceCulling();
    rlDrawRenderBatchActive();
    rlMatrixMode(RL_PROJECTION);
    rlLoadIdentity();
    rlOrtho(0, target->width, 0, target->height, 0.0f, 1.0f);
    rlMatrixMode(RL_MODELVIEW);
    rlLoadIdentity();
    current_ = target;
    currentWidth_ = target->width;
    currentHeight_ = target->height;
    if (clear) ClearBackground(clearColor);
}

void Renderer::endTarget() {
    if (!current_) return;
    EndTextureMode();
    rlDisableBackfaceCulling();
    current_.reset();
}

void Renderer::blit(const ImageBufferPtr &source, float alpha) {
    if (!source || !source->valid() || !current_) return;
    Color tint = WHITE;
    tint.a = static_cast<unsigned char>(std::lround(std::clamp(alpha, 0.0f, 1.0f) * 255.0f));
    DrawTexturePro(source->texture.texture,
                   Rectangle{0, 0, static_cast<float>(source->width),
                             static_cast<float>(source->height)},
                   Rectangle{0, 0, static_cast<float>(currentWidth_),
                             static_cast<float>(currentHeight_)},
                   Vector2{0, 0}, 0.0f, tint);
}

// ---------------------------------------------------------------------------
// Shader passes
// ---------------------------------------------------------------------------

const Renderer::UniformLocs &Renderer::locations(Shader *shader) {
    auto it = locationCache_.find(shader->id);
    if (it != locationCache_.end()) return it->second;
    UniformLocs loc;
    loc.time = GetShaderLocation(*shader, "uTime");
    loc.frame = GetShaderLocation(*shader, "uFrame");
    loc.duration = GetShaderLocation(*shader, "uDuration");
    loc.progress = GetShaderLocation(*shader, "uProgress");
    loc.resolution = GetShaderLocation(*shader, "uResolution");
    loc.texel = GetShaderLocation(*shader, "uTexel");
    loc.bass = GetShaderLocation(*shader, "uBass");
    loc.mid = GetShaderLocation(*shader, "uMid");
    loc.treble = GetShaderLocation(*shader, "uTreble");
    loc.level = GetShaderLocation(*shader, "uLevel");
    loc.onset = GetShaderLocation(*shader, "uOnset");
    loc.beat = GetShaderLocation(*shader, "uBeat");
    loc.user = GetShaderLocation(*shader, "uUser");
    loc.colorA = GetShaderLocation(*shader, "uColorA");
    loc.colorB = GetShaderLocation(*shader, "uColorB");
    loc.vec2 = GetShaderLocation(*shader, "uVector2");
    loc.vec3 = GetShaderLocation(*shader, "uVector3");
    loc.vec4 = GetShaderLocation(*shader, "uVector4");
    loc.matrix = GetShaderLocation(*shader, "uMatrix");
    loc.prev = GetShaderLocation(*shader, "uPrev");
    loc.input2 = GetShaderLocation(*shader, "uInput2");
    loc.feedback = GetShaderLocation(*shader, "uFeedback");
    loc.spectrum = GetShaderLocation(*shader, "uSpectrum");
    loc.waveform = GetShaderLocation(*shader, "uWaveform");
    return locationCache_.emplace(shader->id, loc).first->second;
}

void Renderer::drawShaderPass(Shader *shader, const EvalContext &ctx, Texture2D prev,
                              Texture2D input2, Texture2D feedback, const float *user, int userCount,
                              Color colorA, Color colorB, const ShaderVectorUniforms &vectors) {
    if (!shader || !current_) return;
    const UniformLocs &loc = locations(shader);

    rlDrawRenderBatchActive();
    BeginShaderMode(*shader);
    rlEnableShader(shader->id);

    setFloat(loc.time, static_cast<float>(ctx.time));
    setFloat(loc.frame, static_cast<float>(ctx.frame));
    setFloat(loc.duration, static_cast<float>(ctx.duration));
    setFloat(loc.progress,
             ctx.duration > 0.0 ? std::clamp(static_cast<float>(ctx.time / ctx.duration), 0.0f, 1.0f)
                                : 0.0f);
    setVec2(loc.resolution, static_cast<float>(currentWidth_), static_cast<float>(currentHeight_));
    setVec2(loc.texel, 1.0f / static_cast<float>(std::max(1, currentWidth_)),
            1.0f / static_cast<float>(std::max(1, currentHeight_)));
    setFloat(loc.bass, ctx.bass);
    setFloat(loc.mid, ctx.mid);
    setFloat(loc.treble, ctx.treble);
    setFloat(loc.level, ctx.level);
    setFloat(loc.onset, ctx.onset);
    setFloat(loc.beat, ctx.beat);
    setVec4(loc.colorA, colorA);
    setVec4(loc.colorB, colorB);
    if (vectors.useVec2) setVec2(loc.vec2, vectors.vec2.x, vectors.vec2.y);
    if (vectors.useVec3) setVec3(loc.vec3, vectors.vec3);
    if (vectors.useVec4) setVec4f(loc.vec4, vectors.vec4);
    if (vectors.useMatrix) setMatrix(loc.matrix, vectors.matrix);
    if (userCount > 0) setFloatArray(loc.user, user, std::min(userCount, 8));

    setSampler(loc.prev, prev.id ? prev.id : black_.id);
    setSampler(loc.input2, input2.id ? input2.id : black_.id);
    setSampler(loc.feedback, feedback.id ? feedback.id : black_.id);
    setSampler(loc.spectrum, spectrum_.id);
    setSampler(loc.waveform, waveform_.id);

    DrawTexturePro(white_, Rectangle{0, 0, 1, 1},
                   Rectangle{0, 0, static_cast<float>(currentWidth_),
                             static_cast<float>(currentHeight_)},
                   Vector2{0, 0}, 0.0f, WHITE);
    rlDrawRenderBatchActive();
    EndShaderMode();
    ++stats_.shaderPasses;
}

// ---------------------------------------------------------------------------
// Analysis textures
// ---------------------------------------------------------------------------

void Renderer::uploadAnalysisTextures(const AnalysisData &analysis, double time) {
    const size_t bins = scratchSpectrum_.size();
    const float *row = analysis.spectrumRow(time);
    for (size_t i = 0; i < bins; ++i) {
        const float value = row ? std::clamp(row[i], 0.0f, 1.0f) : 0.0f;
        scratchSpectrum_[i] = value;
    }
    // The texture is read by shaders with bilinear filtering, so storing the
    // value in every channel keeps any component lookup valid.
    for (size_t i = 0; i < bins; ++i) {
        const unsigned char v =
            static_cast<unsigned char>(std::lround(scratchSpectrum_[i] * 255.0f));
        spectrumPixels_[i * 4 + 0] = v;
        spectrumPixels_[i * 4 + 1] = v;
        spectrumPixels_[i * 4 + 2] = v;
        spectrumPixels_[i * 4 + 3] = 255;
    }
    UpdateTexture(spectrum_, spectrumPixels_.data());

    const size_t points = scratchWave_.size();
    const double window = 0.08;  // seconds shown left to right
    const AudioBuffer *source = analysis.source.get();
    if (source && source->frameCount > 0) {
        const double rate = std::max(1, source->sampleRate);
        const double start = time - window * 0.5;
        for (size_t i = 0; i < points; ++i) {
            const double t = start + window * (static_cast<double>(i) / (points - 1));
            const double frame = t * rate - static_cast<double>(source->startFrame);
            scratchWave_[i] = std::clamp(source->monoAt(frame), -1.0f, 1.0f);
        }
    } else {
        std::fill(scratchWave_.begin(), scratchWave_.end(), 0.0f);
    }
    for (size_t i = 0; i < points; ++i) {
        const unsigned char v = static_cast<unsigned char>(
            std::lround(std::clamp(scratchWave_[i] * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f));
        waveformPixels_[i * 4 + 0] = v;
        waveformPixels_[i * 4 + 1] = v;
        waveformPixels_[i * 4 + 2] = v;
        waveformPixels_[i * 4 + 3] = 255;
    }
    UpdateTexture(waveform_, waveformPixels_.data());
}

void Renderer::updateAnalysisTextures(const EvalContext &ctx) {
    if (ctx.analysis) {
        uploadAnalysisTextures(*ctx.analysis, ctx.audioTime);
        return;
    }
    // No global analysis: clear both textures so shaders read silence.
    static const AnalysisData empty;
    uploadAnalysisTextures(empty, ctx.audioTime);
}

// ---------------------------------------------------------------------------
// Beat tracking
// ---------------------------------------------------------------------------

void Renderer::updateBeatPhase(const EvalContext &ctx) {
    if (ctx.onset > 0.35f && ctx.onset > lastOnsetLevel_ * 1.2f) {
        if (lastOnsetTime_ >= 0.0) {
            const double interval = ctx.time - lastOnsetTime_;
            if (interval > 0.20 && interval < 1.5) {
                // exponential moving average keeps the tempo responsive but stable
                beatInterval_ = beatInterval_ * 0.7 + interval * 0.3;
            }
        }
        lastOnsetTime_ = ctx.time;
    }
    lastOnsetLevel_ = ctx.onset;
}

// ---------------------------------------------------------------------------
// Frame rendering
// ---------------------------------------------------------------------------

ImageBufferPtr Renderer::renderFrame(Graph &graph, EvalContext &ctx, std::string *error) {
    const double start = GetTime();
    stats_.shaderPasses = 0;
    ctx.renderer = this;
    ctx.shaders = &shaders_;

    if (ctx.analysis) {
        const float *row = ctx.analysis->spectrumRow(ctx.audioTime);
        if (row) {
            const int bins = ctx.analysis->spectrumBins;
            const int bassEnd = std::max(1, bins / 8);
            const int midEnd = std::max(bassEnd + 1, bins / 2);
            auto average = [&](int from, int to) {
                float sum = 0.0f;
                for (int i = from; i < to; ++i) sum += row[i];
                return to > from ? sum / static_cast<float>(to - from) : 0.0f;
            };
            ctx.bass = average(0, bassEnd);
            ctx.mid = average(bassEnd, midEnd);
            ctx.treble = average(midEnd, bins);
        }
        ctx.level = ctx.analysis->valueAt(ctx.audioTime, 1);
        ctx.onset = ctx.analysis->valueAt(ctx.audioTime, 3);
        ctx.rms = ctx.analysis->valueAt(ctx.audioTime, 0);
    }

    updateBeatPhase(ctx);
    {
        const double phase = beatInterval_ > 0.0
                                 ? (ctx.time - std::max(0.0, lastOnsetTime_)) / beatInterval_
                                 : 0.0;
        ctx.beat = static_cast<float>(phase - std::floor(phase));
    }

    updateAnalysisTextures(ctx);

    if (!graph.evaluate(ctx)) {
        if (error) *error = graph.lastError;
        recycleAll();
        return nullptr;
    }
    stats_.nodesEvaluated = graph.nodeCount();

    ImageBufferPtr result;
    const int sinkId = graph.videoSinkNodeId();
    if (sinkId != 0) {
        const Node *sink = graph.find(sinkId);
        if (sink && sink->enabled) {
            // The output block is a sink: follow its input link to find the image.
            const Link *link = graph.findInputLink(sinkId, 0);
            if (link) {
                const Node *source = graph.find(link->fromNode);
                if (source && source->enabled &&
                    link->fromPort < static_cast<int>(source->outputs.size())) {
                    const Value &value = source->outputs[static_cast<size_t>(link->fromPort)];
                    if (value.image && value.image->valid()) result = value.image;
                }
            }
        }
    }

    if (!result) {
        if (error) {
            *error = sinkId == 0 ? "the project has no Video Output block"
                                 : "the Video Output block has no image connected";
        }
        recycleAll();
        return nullptr;
    }

    // Copy into a stable target so the caller can keep displaying it while the
    // pool is recycled for the next frame.
    if (!output_ || output_->width != ctx.width || output_->height != ctx.height) {
        if (output_ && output_->valid()) UnloadRenderTexture(output_->texture);
        output_ = allocate(ctx.width, ctx.height);
    }
    if (output_) {
        beginTarget(output_, true, Color{0, 0, 0, 255});
        blit(result);
        endTarget();
        result = output_;
    }

    recycleAll();
    stats_.frameMs = (GetTime() - start) * 1000.0;
    return result;
}

}  // namespace pf
