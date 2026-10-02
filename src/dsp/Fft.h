// Iterative radix-2 real FFT with precomputed twiddle factors.
#pragma once

#include <vector>

namespace pf {

class Fft {
public:
    explicit Fft(int size);

    int size() const { return size_; }
    // Computes the magnitude spectrum of `input` (size real samples) into
    // `magnitude` (size/2 values).
    void magnitude(const float *input, float *magnitude) const;

private:
    void transform(float *real, float *imag) const;

    int size_ = 0;
    int half_ = 0;
    std::vector<int> reversed_;
    std::vector<float> cosTable_;
    std::vector<float> sinTable_;
    // scratch buffers for the real-input transform
    mutable std::vector<float> real_;
    mutable std::vector<float> imag_;
};

// Window functions used by the analyser.
void applyHannWindow(float *data, int size);

}  // namespace pf
