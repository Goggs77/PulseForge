// Asynchronous GPU->CPU frame readback through a pixel buffer object ring.
#pragma once

#include <cstddef>

#include "raylib.h"

namespace pf {

class FrameReadback {
public:
    ~FrameReadback();

    void shutdown();
    // Enqueues a readback of the given render target.
    void capture(RenderTexture2D target);
    // Returns the frame captured by the *previous* call (top-down RGBA rows),
    // or nullptr when no previous frame exists yet. The pointer stays valid
    // until release() is called.
    const unsigned char *fetch();
    void release();

    int width() const { return width_; }
    int height() const { return height_; }
    size_t byteSize() const { return size_; }
    long long framesProduced() const { return frames_; }

private:
    void ensure(int width, int height);

    unsigned int pbo_[2] = {0, 0};
    int lastSlot_ = -1;
    int mappedSlot_ = -1;
    int width_ = 0;
    int height_ = 0;
    size_t size_ = 0;
    long long frames_ = 0;
};

}  // namespace pf
