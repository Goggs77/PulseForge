#include "render/FrameReadback.h"

#include <glad.h>

#include "rlgl.h"

namespace pf {

FrameReadback::~FrameReadback() { shutdown(); }

void FrameReadback::shutdown() {
    if (mappedSlot_ >= 0) release();
    if (pbo_[0] || pbo_[1]) {
        glDeleteBuffers(2, pbo_);
        pbo_[0] = pbo_[1] = 0;
    }
    lastSlot_ = -1;
    frames_ = 0;
}

void FrameReadback::ensure(int width, int height) {
    if (width == width_ && height == height_ && pbo_[0] != 0) return;
    shutdown();
    width_ = width;
    height_ = height;
    size_ = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
    glGenBuffers(2, pbo_);
    for (int i = 0; i < 2; ++i) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_[i]);
        glBufferData(GL_PIXEL_PACK_BUFFER, static_cast<GLsizeiptr>(size_), nullptr, GL_STREAM_READ);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
}

void FrameReadback::capture(RenderTexture2D target) {
    if (target.id == 0) return;
    if (mappedSlot_ >= 0) release();
    ensure(target.texture.width, target.texture.height);
    if (pbo_[0] == 0) return;

    rlDrawRenderBatchActive();
    const int slot = (lastSlot_ + 1) % 2;
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_[slot]);
    glBindFramebuffer(GL_FRAMEBUFFER, target.id);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    // The renderer keeps targets in the normalised convention, so the rows come
    // back top-down and can be piped straight into ffmpeg.
    glReadPixels(0, 0, width_, height_, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    lastSlot_ = slot;
    ++frames_;
}

const unsigned char *FrameReadback::fetch() {
    if (frames_ < 2 || lastSlot_ < 0) return nullptr;
    if (mappedSlot_ >= 0) release();
    const int slot = (lastSlot_ + 1) % 2;  // written by the previous capture
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_[slot]);
    void *ptr = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, static_cast<GLsizeiptr>(size_),
                                 GL_MAP_READ_BIT);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    if (!ptr) return nullptr;
    mappedSlot_ = slot;
    return static_cast<const unsigned char *>(ptr);
}

void FrameReadback::release() {
    if (mappedSlot_ < 0) return;
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_[mappedSlot_]);
    glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    mappedSlot_ = -1;
}

}  // namespace pf
