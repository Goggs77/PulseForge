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
    // The monitor plays the signal that reaches the Audio Output. With no
    // override the decoded clip is streamed; `setPlaybackBuffer(nullptr)` makes
    // playback silent without opening a stream, and a rendered buffer streams
    // the graph-processed track instead. Positions stay in clip time: the
    // buffer's `startFrame` maps them onto its samples.
    void setPlaybackBuffer(const AudioPtr &buffer);
    void clearPlaybackBuffer();
    bool hasPlaybackOverride() const { return playbackOverride_; }

    // ---- live graph audio ---------------------------------------------------
    // Playback of the graph-rendered Audio Output: the app pushes each video
    // frame's samples (with their absolute clip start frame) and the stream
    // feeds them to the device in whole sub-buffers. This keeps the audio chain
    // in step with the video chain instead of pre-rendering the whole track.
    // `frameSamples` is the number of audio samples one video frame produces;
    // the stream sub-buffer is sized from it so the device cannot drain the
    // whole ring between two video frames.
    void startLiveStream(int sampleRate, int channels, int frameSamples = 1024);
    void stopLiveStream();
    bool liveStreamActive() const { return liveStream_; }
    void pushLiveWindow(const AudioBuffer &buffer);

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
    void feedLive();
    void clearLiveQueue();
    const AudioBuffer *playbackBuffer() const;
    int playbackSampleRate() const;
    // Absolute clip position <-> index inside the playback buffer.
    long long frameForPosition(double seconds) const;
    double positionForFrame(long long frame) const;

    AudioPtr buffer_;
    AudioPtr playback_;
    bool playbackOverride_ = false;
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

    bool liveStream_ = false;
    bool liveStarted_ = false;
    int liveRate_ = 48000;
    int liveChannels_ = 2;
    int liveSubBufferFrames_ = 1024;
    long long liveNextFrame_ = -1;  // absolute clip frame already queued
    long long liveQueuedFrames_ = 0;
    size_t liveReadFrame_ = 0;
    std::vector<float> liveQueue_;
};

}  // namespace pf
