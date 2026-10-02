// Decoded audio plus a preview playback stream.
#pragma once

#include <string>
#include <vector>

#include "core/Port.h"

namespace pf {

class AudioClip {
public:
    ~AudioClip();

    // Decodes anything ffmpeg can read into interleaved float samples.
    bool load(const std::string &path, int sampleRate, std::string *error);
    void clear();
    bool valid() const { return buffer_ && buffer_->frameCount > 0; }

    const AudioPtr &buffer() const { return buffer_; }
    const std::string &path() const { return path_; }
    double duration() const { return buffer_ ? buffer_->duration() : 0.0; }
    int channels() const { return buffer_ ? buffer_->channels : 0; }
    int sampleRate() const { return buffer_ ? buffer_->sampleRate : 0; }

    // Media that could not be matched with a compatible encoder is transcoded to
    // AAC once and kept here, so the import can proceed without writing a
    // converted file next to the source.
    bool hasTranscodedAudio() const { return !transcodedAac_.empty(); }
    const std::vector<unsigned char> &transcodedAudio() const { return transcodedAac_; }
    void setTranscodedAudio(std::vector<unsigned char> bytes) {
        transcodedAac_ = std::move(bytes);
    }
    void clearTranscodedAudio() { transcodedAac_.clear(); }

    // Min/max overview used by the timeline widget.
    void buildOverview(int buckets = 4096);
    int overviewBuckets() const { return static_cast<int>(overviewMin_.size()); }
    float overviewMin(int index) const;
    float overviewMax(int index) const;

    // ---- preview playback -------------------------------------------------
    bool audioDeviceReady() const;
    void startPreview();
    void pausePreview();
    void stopPreview();
    bool previewPlaying() const { return playing_; }
    void seek(double seconds);
    double previewPosition() const { return position_; }
    void updatePreview();

private:
    void ensureStream();
    void destroyStream();
    void feed();

    AudioPtr buffer_;
    std::string path_;
    std::vector<unsigned char> transcodedAac_;
    std::vector<float> overviewMin_;
    std::vector<float> overviewMax_;

    AudioStream stream_{};
    bool streamReady_ = false;
    bool playing_ = false;
    double position_ = 0.0;
    double startClock_ = 0.0;
    double startPosition_ = 0.0;
    long long feedFrame_ = 0;
    long long startFeedFrame_ = 0;
};

}  // namespace pf
