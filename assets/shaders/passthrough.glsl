// Passthrough - the smallest useful PulseForge shader, and the starting point
// for your own effects.
//
// A file that does not start with #version gets the PulseForge preamble
// prepended, so all of this is already declared:
//   pfUv()                 (0,0) top-left .. (1,1) bottom-right
//   pfPrev(uv)             the upstream Image (uPrev)
//   pfInput2(uv)           the second Image input (uInput2)
//   pfFeedback(uv)         this block's previous frame (turn Feedback on)
//   pfSpectrum(x)          1D log-frequency spectrum, 0..1
//   pfSpectrumBand(lo, hi) averaged band, pfWave(x) waveform -1..1
//   uTime, uFrame, uDuration, uProgress, uResolution, uTexel
//   uBass, uMid, uTreble, uLevel, uOnset, uBeat
//   uUser[0..7], uColorA, uColorB, uVector2/3/4, uMatrix
//
// Pick the file in a Shader block: the block's input ports are derived from the
// uniforms this file actually uses (here just uPrev), so a shader only shows
// the knobs it needs.

void main() {
    finalColor = vec4(pfPrev(pfUv()).rgb, 1.0);
}
