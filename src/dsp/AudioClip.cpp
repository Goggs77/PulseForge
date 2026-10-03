#include "dsp/AudioClip.h"

#include <algorithm>
#include <cmath>

#include "dsp/Analysis.h"
#include "export/FFmpeg.h"
#include "raylib.h"

namespace pf {

namespace {
// raylib streams through a double buffer: UpdateAudioStream() always writes a
// whole sub-buffer and zero-fills whatever is left over, so a chunk shorter
// than one sub-buffer turns into audible "half sound, half silence" pulsing.
// SetAudioStreamBufferSizeDefault() selects that sub-buffer size (the ring is
// allocated at twice this value), so the feed chunk has to match it exactly.
constexpr int kPreviewSubBufferFrames = 2048;  // ~43 ms at 48 kHz
}  // namespace

AudioClip::~AudioClip() { destroyStream(); }

bool AudioClip::load(const std::string &path, int sampleRate, std::string *error) {
    // The new file may use another sample rate, and the old stream is bound to
    // the previous one, so playback is torn down first.
    stopPreview();
    destroyStream();
    buffer_.reset();
    playback_.reset();
    playbackOverride_ = false;
    transcodedAac_.clear();
    position_ = 0.0;
    startPosition_ = 0.0;
    feedFrame_ = 0;
    startFeedFrame_ = 0;
    std::vector<float> samples;
    int channels = 0;
    if (!ffmpeg::decodeAudioFloat(path, sampleRate, &channels, &samples, error)) {
        return false;
    }
    auto buffer = std::make_shared<AudioBuffer>();
    buffer->channels = std::max(1, channels);
    buffer->sampleRate = sampleRate;
    buffer->samples = std::move(samples);
    buffer->frameCount = static_cast<long long>(buffer->samples.size()) /
                         std::max(1, buffer->channels);
    buffer_ = buffer;
    path_ = path;
    buildOverview();
    return true;
}

void AudioClip::clear() {
    stopPreview();
    destroyStream();
    buffer_.reset();
    playback_.reset();
    playbackOverride_ = false;
    transcodedAac_.clear();
    path_.clear();
    overviewMin_.clear();
    overviewMax_.clear();
}

void AudioClip::buildOverview(int buckets) {
    overviewMin_.clear();
    overviewMax_.clear();
    if (!valid() || buckets <= 0) return;
    buckets = std::min(buckets, static_cast<int>(buffer_->frameCount));
    overviewMin_.assign(static_cast<size_t>(buckets), 0.0f);
    overviewMax_.assign(static_cast<size_t>(buckets), 0.0f);
    const double perBucket = static_cast<double>(buffer_->frameCount) / buckets;
    for (int b = 0; b < buckets; ++b) {
        const long long from = static_cast<long long>(b * perBucket);
        const long long to = std::min<long long>(buffer_->frameCount,
                                                 static_cast<long long>((b + 1) * perBucket) + 1);
        float lo = 0.0f, hi = 0.0f;
        if (to > from) {
            lo = 1e9f;
            hi = -1e9f;
            for (long long i = from; i < to; ++i) {
                const float v = buffer_->monoAt(static_cast<double>(i));
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
        }
        if (lo > hi) {
            lo = hi = 0.0f;
        }
        overviewMin_[static_cast<size_t>(b)] = lo;
        overviewMax_[static_cast<size_t>(b)] = hi;
    }
}

float AudioClip::overviewMin(int index) const {
    if (index < 0 || index >= static_cast<int>(overviewMin_.size())) return 0.0f;
    return overviewMin_[static_cast<size_t>(index)];
}

float AudioClip::overviewMax(int index) const {
    if (index < 0 || index >= static_cast<int>(overviewMax_.size())) return 0.0f;
    return overviewMax_[static_cast<size_t>(index)];
}

bool AudioClip::audioDeviceReady() const { return IsAudioDeviceReady(); }

const AudioBuffer *AudioClip::playbackBuffer() const {
    if (playbackOverride_) return playback_.get();
    return buffer_.get();
}

int AudioClip::playbackSampleRate() const {
    const AudioBuffer *buffer = playbackBuffer();
    return buffer ? std::max(1, buffer->sampleRate) : 48000;
}

long long AudioClip::frameForPosition(double seconds) const {
    const AudioBuffer *buffer = playbackBuffer();
    if (!buffer || buffer->frameCount <= 0) return 0;
    const long long frame = std::llround(seconds * playbackSampleRate()) - buffer->startFrame;
    return std::clamp<long long>(frame, 0, buffer->frameCount);
}

double AudioClip::positionForFrame(long long frame) const {
    const AudioBuffer *buffer = playbackBuffer();
    if (!buffer) return 0.0;
    const long long clamped = std::clamp<long long>(frame, 0, buffer->frameCount);
    return static_cast<double>(clamped + buffer->startFrame) / playbackSampleRate();
}

void AudioClip::setPlaybackBuffer(const AudioPtr &buffer) {
    playing_ = false;
    destroyStream();
    playbackOverride_ = true;
    playback_ = buffer;
    feedFrame_ = frameForPosition(position_);
    startFeedFrame_ = feedFrame_;
    startClock_ = GetTime();
    startPosition_ = position_;
}

void AudioClip::clearPlaybackBuffer() {
    playing_ = false;
    destroyStream();
    playbackOverride_ = false;
    playback_.reset();
    feedFrame_ = frameForPosition(position_);
    startFeedFrame_ = feedFrame_;
    startClock_ = GetTime();
    startPosition_ = position_;
}

void AudioClip::ensureStream() {
    const AudioBuffer *playback = playbackBuffer();
    if (streamReady_ || !playback || playback->frameCount <= 0 || !audioDeviceReady()) return;
    SetAudioStreamBufferSizeDefault(kPreviewSubBufferFrames);
    stream_ = LoadAudioStream(static_cast<unsigned int>(std::max(1, playback->sampleRate)), 32,
                              static_cast<unsigned int>(std::max(1, playback->channels)));
    streamReady_ = stream_.buffer != nullptr;
}

void AudioClip::destroyStream() {
    if (streamReady_) {
        StopAudioStream(stream_);
        UnloadAudioStream(stream_);
        streamReady_ = false;
    }
    stream_ = AudioStream{};
}

void AudioClip::feed() {
    const AudioBuffer *playback = playbackBuffer();
    if (!streamReady_ || !playback) return;
    while (IsAudioStreamProcessed(stream_)) {
        const long long remaining = playback->frameCount - feedFrame_;
        if (remaining <= 0) return;
        // Exactly one sub-buffer per call: raylib zero-fills the remainder, so
        // anything shorter would insert trailing silence.
        const int frames = static_cast<int>(std::min<long long>(kPreviewSubBufferFrames, remaining));
        const float *base =
            playback->samples.data() +
            static_cast<size_t>(feedFrame_) * static_cast<size_t>(playback->channels);
        UpdateAudioStream(stream_, base, frames);
        feedFrame_ += frames;
        if (frames < kPreviewSubBufferFrames) return;  // final, partial chunk
    }
}

void AudioClip::startPreview() {
    const AudioBuffer *playback = playbackBuffer();
    if (!playback || playback->frameCount <= 0 || !audioDeviceReady()) return;
    ensureStream();
    if (!streamReady_) return;
    if (playing_) return;
    // PlayAudioStream() resets the ring cursors and marks both sub-buffers as
    // free, so the stream has to be primed after it, not before.
    PlayAudioStream(stream_);
    playing_ = true;
    startClock_ = GetTime();
    startPosition_ = position_;
    startFeedFrame_ = feedFrame_;
    feed();
}

void AudioClip::pausePreview() {
    // Pausing keeps the position, but the ring is left holding the tail of the
    // old queue, so resume restarts the stream from the stopped position.
    playing_ = false;
    if (streamReady_) StopAudioStream(stream_);
    feedFrame_ = frameForPosition(position_);
    startFeedFrame_ = feedFrame_;
    startClock_ = GetTime();
    startPosition_ = position_;
}

void AudioClip::stopPreview() {
    playing_ = false;
    if (streamReady_) StopAudioStream(stream_);
    feedFrame_ = frameForPosition(position_);
    startFeedFrame_ = feedFrame_;
    startClock_ = GetTime();
    startPosition_ = position_;
}

void AudioClip::seek(double seconds) {
    const AudioBuffer *playback = playbackBuffer();
    const double end = playback ? positionForFrame(playback->frameCount)
                                : (buffer_ ? buffer_->duration() : std::max(0.0, seconds));
    const bool wasPlaying = playing_;
    playing_ = false;
    if (streamReady_) StopAudioStream(stream_);
    position_ = std::clamp(seconds, 0.0, std::max(0.0, end));
    feedFrame_ = frameForPosition(position_);
    startFeedFrame_ = feedFrame_;
    startClock_ = GetTime();
    startPosition_ = position_;
    if (wasPlaying) startPreview();
}

void AudioClip::updatePreview() {
    const AudioBuffer *playback = playbackBuffer();
    if (!playback) return;  // silent output: the app clock drives the playhead
    if (playing_) {
        // The playhead follows the frames actually handed to the device, never
        // the wall clock alone, so a stalled frame cannot run ahead of the
        // sound.
        const double elapsed = GetTime() - startClock_;
        const double queued =
            static_cast<double>(feedFrame_ - startFeedFrame_) / playbackSampleRate();
        position_ = startPosition_ + std::min(elapsed, queued);
        if (feedFrame_ >= playback->frameCount && elapsed >= queued) {
            position_ = positionForFrame(playback->frameCount);
            playing_ = false;
            if (streamReady_) StopAudioStream(stream_);
        }
    }
    if (playing_) feed();
}

}  // namespace pf
