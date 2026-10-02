// Thin wrappers around the ffmpeg/ffprobe executables: audio decoding, media
// probing and a long lived encoder process that consumes raw frames on stdin.
#pragma once

#include <cstddef>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pf {

struct MediaInfo {
    bool ok = false;
    double duration = 0.0;
    int sampleRate = 0;
    int channels = 0;
    long long bitRate = 0;  // bits per second, -1/0 when the container has none
    std::string codec;
    std::string format;
    std::string error;
    long long fileSize = -1;
};

namespace ffmpeg {

std::string ffmpegPath();
std::string ffprobePath();
bool available();

// Decodes any file ffmpeg understands into interleaved 32-bit float samples.
// channels is clamped to 1..2 and the stream is resampled to `sampleRate`.
bool decodeAudioFloat(const std::string &path, int sampleRate, int *outChannels,
                      std::vector<float> *outSamples, std::string *error);

// True when this ffmpeg build carries an encoder with that name (cached).
bool hasEncoder(const std::string &name);

// Encodes interleaved 32-bit float samples to an AAC (ADTS) stream held in
// memory. Used when the imported media cannot be re-encoded with a matching
// encoder, so the import can still proceed without asking for a converted file.
bool encodeAacInMemory(const float *samples, long long frameCount, int channels,
                       int inputSampleRate, int outputSampleRate, int bitrateKbps,
                       std::vector<unsigned char> *out, std::string *error);

MediaInfo probe(const std::string &path);

}  // namespace ffmpeg

// A child process with an anonymous stdin pipe and captured stdout/stderr.
class ChildProcess {
public:
    ChildProcess() = default;
    ~ChildProcess();
    ChildProcess(const ChildProcess &) = delete;
    ChildProcess &operator=(const ChildProcess &) = delete;

    // `args` excludes the program name; it is taken from args[0].
    bool start(const std::vector<std::string> &args, std::string *error);
    bool write(const void *data, size_t size);
    bool closeStdin();
    // Waits for exit; returns true when the exit code was zero.
    bool wait(int *exitCode = nullptr);
    bool running() const { return running_; }
    void terminate();
    // Everything the child wrote to stdout (decoded PCM, probe JSON, ...).
    std::string stdoutText() const;
    std::string stderrText() const;

private:
    void *handle_ = nullptr;   // PROCESS_INFORMATION*
    void *stdinWrite_ = nullptr;
    void *stderrRead_ = nullptr;
    void *stdoutRead_ = nullptr;
    bool running_ = false;
    int exitCode_ = -1;
    std::thread reader_;
    std::thread stdoutReader_;
    mutable std::mutex textMutex_;
    std::string stderrText_;
    std::string stdoutText_;
};

}  // namespace pf
