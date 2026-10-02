# Rendering conventions (verified on this machine)

Everything below was measured with `tools/probe.cpp` on
NVIDIA GeForce GTX 1660 Ti / OpenGL 3.3.0, and the renderer depends on it.

## raylib's default render-target convention

`BeginTextureMode()` sets `rlOrtho(0, w, h, 0, 0, 1)`, which places raylib's
screen `y = 0` (the visual top) at texture coordinate `v = 1`. Measured with
`glReadPixels`, the rows come back bottom-up relative to the visual image, and a
plain `texture(uPrev, uv)` in a shader reads the image upside down.

## PulseForge's normalised convention

Immediately after `BeginTextureMode()` the renderer calls

```c
rlMatrixMode(RL_PROJECTION);
rlLoadIdentity();
rlOrtho(0, w, 0, h, 0.0f, 1.0f);
rlMatrixMode(RL_MODELVIEW);
rlLoadIdentity();
```

which puts the visual top at `v = 0`. Consequences, all verified:

* shaders sample upstream images with the plain `texture(uPrev, uv)` and get an
  upright image, with `uv = (0,0)` at the top-left;
* a shader passthrough into another target preserves orientation;
* geometry drawn with raylib's y-down primitives after a shader pass lands where
  the author expects;
* `glReadPixels` returns rows top-down, exactly what `ffmpeg -f rawvideo
  -pix_fmt rgba` expects, so exported frames need no flip;
* displaying a target with `DrawTexturePro(src = {0, 0, w, +h})` is upright
  (note: the raylib idiom of a negative source height would flip it).

## Backface culling

Flipping the projection reverses triangle winding. raylib has backface culling
enabled, so **every** draw into a render target must be preceded by
`rlDisableBackfaceCulling()`. Without it the frame silently comes out empty
(this cost an hour).

## Readback

`glReadPixels` through a pixel buffer object works and produces byte-identical
results to a synchronous read. PulseForge keeps a two-buffer PBO ring so the GPU
copy of frame *n* overlaps with the CPU upload of frame *n-1* to ffmpeg.
