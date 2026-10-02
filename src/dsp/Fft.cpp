#include "dsp/Fft.h"

#include <algorithm>
#include <cmath>

namespace pf {

namespace {
constexpr float kTwoPi = 6.28318530717958647692f;
}

Fft::Fft(int size) : size_(size), half_(size / 2) {
    reversed_.resize(static_cast<size_t>(half_));
    cosTable_.resize(static_cast<size_t>(half_ / 2));
    sinTable_.resize(static_cast<size_t>(half_ / 2));
    real_.resize(static_cast<size_t>(half_));
    imag_.resize(static_cast<size_t>(half_));

    int bits = 0;
    while ((1 << bits) < half_) ++bits;
    for (int i = 0; i < half_; ++i) {
        int value = i;
        int reversed = 0;
        for (int b = 0; b < bits; ++b) {
            reversed = (reversed << 1) | (value & 1);
            value >>= 1;
        }
        reversed_[static_cast<size_t>(i)] = reversed;
    }
    for (int i = 0; i < half_ / 2; ++i) {
        const float angle = -kTwoPi * static_cast<float>(i) / static_cast<float>(half_);
        cosTable_[static_cast<size_t>(i)] = std::cos(angle);
        sinTable_[static_cast<size_t>(i)] = std::sin(angle);
    }
}

void Fft::transform(float *real, float *imag) const {
    const int n = half_;
    for (int i = 0; i < n; ++i) {
        const int j = reversed_[static_cast<size_t>(i)];
        if (j > i) {
            std::swap(real[i], real[j]);
            std::swap(imag[i], imag[j]);
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const int step = n / len;
        const int halfLen = len / 2;
        for (int i = 0; i < n; i += len) {
            for (int j = 0; j < halfLen; ++j) {
                const int tableIndex = j * step;
                const float wr = cosTable_[static_cast<size_t>(tableIndex)];
                const float wi = sinTable_[static_cast<size_t>(tableIndex)];
                const int a = i + j;
                const int b = a + halfLen;
                const float tr = real[b] * wr - imag[b] * wi;
                const float ti = real[b] * wi + imag[b] * wr;
                real[b] = real[a] - tr;
                imag[b] = imag[a] - ti;
                real[a] += tr;
                imag[a] += ti;
            }
        }
    }
}

void Fft::magnitude(const float *input, float *magnitude) const {
    const int n = half_;
    // Pack the real signal as n complex samples: z[k] = x[2k] + i*x[2k+1].
    for (int i = 0; i < n; ++i) {
        real_[static_cast<size_t>(i)] = input[2 * i];
        imag_[static_cast<size_t>(i)] = input[2 * i + 1];
    }
    transform(real_.data(), imag_.data());

    // Unpack the N/2 complex transform into the first N/2 bins of the real one.
    for (int k = 0; k <= n / 2; ++k) {
        const int j = (n - k) % n;
        const float ar = real_[static_cast<size_t>(k)];
        const float ai = imag_[static_cast<size_t>(k)];
        const float br = real_[static_cast<size_t>(j)];
        const float bi = -imag_[static_cast<size_t>(j)];
        const float sumR = ar + br, sumI = ai + bi;
        const float difR = ar - br, difI = ai - bi;
        const float angle = -kTwoPi * static_cast<float>(k) / static_cast<float>(size_);
        const float wr = std::cos(angle);
        const float wi = std::sin(angle);
        const float xr = 0.5f * (sumR + (difR * wi + difI * wr));
        const float xi = 0.5f * (sumI - (difR * wr - difI * wi));
        magnitude[k] = std::sqrt(xr * xr + xi * xi);
    }
    // The upper half mirrors the lower half for real input.
    for (int k = n / 2 + 1; k < n; ++k) {
        magnitude[k] = magnitude[n - k];
    }
}

void applyHannWindow(float *data, int size) {
    if (size <= 1) return;
    const float scale = kTwoPi / static_cast<float>(size - 1);
    for (int i = 0; i < size; ++i) {
        data[i] *= 0.5f * (1.0f - std::cos(scale * static_cast<float>(i)));
    }
}

}  // namespace pf
