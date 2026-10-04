#include "export/FFmpeg.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_map>

#include "core/Json.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace pf {

namespace {

std::string quoteArgument(const std::string &argument) {
    // Windows command line quoting rules (the same ones CommandLineToArgvW uses).
    if (!argument.empty() && argument.find_first_of(" \t\n\v\"") == std::string::npos) {
        return argument;
    }
    std::string result = "\"";
    for (auto it = argument.begin();; ++it) {
        unsigned int backslashes = 0;
        while (it != argument.end() && *it == '\\') {
            ++it;
            ++backslashes;
        }
        if (it == argument.end()) {
            result.append(backslashes * 2, '\\');
            break;
        }
        if (*it == '"') {
            result.append(backslashes * 2 + 1, '\\');
        } else {
            result.append(backslashes, '\\');
        }
        result.push_back(*it);
    }
    result.push_back('"');
    return result;
}

std::string buildCommandLine(const std::vector<std::string> &arguments) {
    std::string line;
    for (size_t i = 0; i < arguments.size(); ++i) {
        if (i) line.push_back(' ');
        line += quoteArgument(arguments[i]);
    }
    return line;
}

bool fileExists(const std::string &path) {
    if (path.empty()) return false;
    FILE *file = std::fopen(path.c_str(), "rb");
    if (!file) return false;
    std::fclose(file);
    return true;
}

std::string searchOnPath(const std::string &executable) {
#ifdef _WIN32
    char buffer[MAX_PATH] = {0};
    if (SearchPathA(nullptr, executable.c_str(), nullptr, MAX_PATH, buffer, nullptr) > 0) {
        return std::string(buffer);
    }
#endif
    return std::string();
}

}  // namespace

std::string ffmpeg::ffmpegPath() {
    static std::string cached;
    if (!cached.empty()) return cached;
#ifdef PF_FFMPEG_DEFAULT
    const std::string configured = PF_FFMPEG_DEFAULT;
    if (fileExists(configured)) {
        cached = configured;
        return cached;
    }
#endif
    for (const char *name : {"ffmpeg.exe", "ffmpeg"}) {
        const std::string found = searchOnPath(name);
        if (!found.empty()) {
            cached = found;
            return cached;
        }
    }
    cached = "ffmpeg";
    return cached;
}

std::string ffmpeg::ffprobePath() {
    static std::string cached;
    if (!cached.empty()) return cached;
#ifdef PF_FFPROBE_DEFAULT
    const std::string configured = PF_FFPROBE_DEFAULT;
    if (fileExists(configured)) {
        cached = configured;
        return cached;
    }
#endif
    for (const char *name : {"ffprobe.exe", "ffprobe"}) {
        const std::string found = searchOnPath(name);
        if (!found.empty()) {
            cached = found;
            return cached;
        }
    }
    cached = "ffprobe";
    return cached;
}

bool ffmpeg::available() {
    return !searchOnPath("ffmpeg.exe").empty() || fileExists(ffmpegPath()) ||
           ffmpegPath().find(':') != std::string::npos;
}

namespace {

// `ffmpeg -encoders` lists one entry per line as
//   " A....D libmp3lame   libmp3lame MP3 (MPEG audio layer 3)"
// The type letter and its flags form the first token, so the encoder name is
// the second whitespace separated token.
const std::set<std::string> &knownEncoders() {
    static const std::set<std::string> names = [] {
        std::set<std::string> result;
        std::vector<std::string> arguments = {ffmpeg::ffmpegPath(), "-hide_banner", "-loglevel",
                                              "error", "-encoders"};
        ChildProcess process;
        if (!process.start(arguments, nullptr) || !process.wait(nullptr)) return result;
        std::istringstream stream(process.stdoutText());
        std::string line;
        while (std::getline(stream, line)) {
            const size_t first = line.find_first_not_of(" \t");
            if (first == std::string::npos) continue;
            const char kind = line[first];
            if (kind != 'V' && kind != 'A' && kind != 'S') continue;
            const size_t flagsEnd = line.find_first_of(" \t", first);
            if (flagsEnd == std::string::npos) continue;
            const size_t nameStart = line.find_first_not_of(" \t", flagsEnd);
            if (nameStart == std::string::npos) continue;
            const size_t nameEnd = line.find_first_of(" \t", nameStart);
            result.insert(line.substr(nameStart, nameEnd - nameStart));
        }
        return result;
    }();
    return names;
}

}  // namespace

bool ffmpeg::hasEncoder(const std::string &name) {
    if (name.empty()) return false;
    return knownEncoders().count(name) > 0;
}

bool ffmpeg::canRunVideoEncoder(const std::string &name) {
    if (name.empty()) return false;
    static std::unordered_map<std::string, bool> cache;
    const auto it = cache.find(name);
    if (it != cache.end()) return it->second;

    bool ok = false;
    if (hasEncoder(name)) {
        const std::vector<std::string> arguments = {
            ffmpegPath(), "-hide_banner", "-nostdin", "-loglevel", "error",
            "-f",         "lavfi",        "-i",       "color=black:s=320x240:r=30:d=0.5",
            "-frames:v",  "1",            "-pix_fmt", "yuv420p",
            "-c:v",       name,           "-f",       "null",
            "-"};
        ChildProcess probe;
        if (probe.start(arguments, nullptr)) {
            int exitCode = 0;
            ok = probe.wait(&exitCode) && exitCode == 0;
        }
    }
    cache[name] = ok;
    return ok;
}

bool ffmpeg::encodeAacInMemory(const float *samples, long long frameCount, int channels,
                               int inputSampleRate, int outputSampleRate, int bitrateKbps,
                               std::vector<unsigned char> *out, std::string *error) {
    if (out) out->clear();
    if (!samples || frameCount <= 0 || channels <= 0 || inputSampleRate <= 0) {
        if (error) *error = "nothing to encode";
        return false;
    }
    if (outputSampleRate <= 0) outputSampleRate = inputSampleRate;
    const std::vector<std::string> arguments = {
        ffmpegPath(),  "-hide_banner", "-nostdin", "-loglevel", "error",
        "-f",          "f32le",        "-ac",      std::to_string(channels),
        "-ar",         std::to_string(inputSampleRate),
        "-i",          "-",            "-c:a",     "aac",
        "-b:a",        std::to_string(std::max(1, bitrateKbps)) + "k",
        "-ar",         std::to_string(outputSampleRate),
        "-f",          "adts",         "-"};
    ChildProcess process;
    if (!process.start(arguments, error)) return false;
    const size_t total = static_cast<size_t>(frameCount) * static_cast<size_t>(channels) *
                         sizeof(float);
    const unsigned char *bytes = reinterpret_cast<const unsigned char *>(samples);
    constexpr size_t kChunk = 1u << 20;
    for (size_t written = 0; written < total; written += kChunk) {
        const size_t chunk = std::min(kChunk, total - written);
        if (!process.write(bytes + written, chunk)) {
            process.closeStdin();
            process.wait(nullptr);
            if (error) {
                const std::string detail = process.stderrText();
                *error = "ffmpeg stopped while encoding AAC" +
                         (detail.empty() ? std::string() : ": " + detail);
            }
            return false;
        }
    }
    process.closeStdin();
    int exitCode = 0;
    if (!process.wait(&exitCode) || exitCode != 0) {
        if (error) *error = "ffmpeg could not encode AAC: " + process.stderrText();
        return false;
    }
    const std::string data = process.stdoutText();
    if (data.size() < 128) {
        if (error) *error = "ffmpeg produced an empty AAC stream";
        return false;
    }
    if (out) out->assign(data.begin(), data.end());
    return true;
}

bool ffmpeg::decodeAudioFloat(const std::string &path, int sampleRate, int *outChannels,
                              std::vector<float> *outSamples, std::string *error) {
    if (outSamples) outSamples->clear();
    if (!fileExists(path)) {
        if (error) *error = "audio file not found: " + path;
        return false;
    }
    std::vector<std::string> arguments = {
        ffmpegPath(),       "-hide_banner", "-nostdin", "-loglevel", "error",
        "-i",               path,           "-vn",       "-f",        "f32le",
        "-ac",              "2",            "-ar",       std::to_string(sampleRate),
        "-"};

    ChildProcess process;
    if (!process.start(arguments, error)) return false;
    // The child writes the PCM stream to stdout: capture it all, then wait.
    if (!process.wait(nullptr)) {
        if (error) *error = "ffmpeg failed to decode audio";
        return false;
    }
    const std::string pcm = process.stdoutText();
    if (pcm.size() % (sizeof(float) * 2) != 0) {
        if (error) *error = "ffmpeg returned a truncated PCM stream";
        return false;
    }
    if (outSamples) {
        outSamples->resize(pcm.size() / sizeof(float));
        std::memcpy(outSamples->data(), pcm.data(), pcm.size());
    }
    if (outChannels) *outChannels = 2;
    return true;
}

MediaInfo ffmpeg::probe(const std::string &path) {
    MediaInfo info;
    std::vector<std::string> arguments = {
        ffprobePath(), "-hide_banner", "-v", "error", "-print_format", "json", "-show_format",
        "-show_streams", path};
    ChildProcess process;
    if (!process.start(arguments, &info.error)) return info;
    if (!process.wait(nullptr)) {
        info.error = "ffprobe failed";
        return info;
    }
    json::Value root;
    if (!json::parse(process.stdoutText(), root, &info.error)) return info;
    const json::Value &format = root["format"];
    info.duration = format["duration"].asDouble(0.0);
    info.format = format["format_name"].asString();
    info.bitRate = static_cast<long long>(format["bit_rate"].asDouble(0.0));
    const json::Value &streams = root["streams"];
    for (size_t i = 0; i < streams.size(); ++i) {
        const json::Value &stream = streams.at(i);
        if (stream["codec_type"].asString() == "video" && info.videoCodec.empty()) {
            info.videoCodec = stream["codec_name"].asString();
            info.videoWidth = stream["width"].asInt(0);
            info.videoHeight = stream["height"].asInt(0);
            // "10/1" style rationals; 0/0 for a still image.
            const auto parseRate = [](const std::string &text) {
                const size_t slash = text.find('/');
                if (slash == std::string::npos) return std::atof(text.c_str());
                const double numerator = std::atof(text.substr(0, slash).c_str());
                const double denominator = std::atof(text.substr(slash + 1).c_str());
                return denominator > 0.0 ? numerator / denominator : 0.0;
            };
            info.videoFps = parseRate(stream["avg_frame_rate"].asString("0/0"));
            if (info.videoFps <= 0.0) {
                info.videoFps = parseRate(stream["r_frame_rate"].asString("0/0"));
            }
        }
        if (stream["codec_type"].asString() != "audio") continue;
        info.sampleRate = std::atoi(stream["sample_rate"].asString("0").c_str());
        info.channels = stream["channels"].asInt(0);
        info.codec = stream["codec_name"].asString();
        const long long streamBitRate = static_cast<long long>(stream["bit_rate"].asDouble(0.0));
        if (streamBitRate > 0) info.bitRate = streamBitRate;
        break;
    }
    info.ok = info.duration > 0.0;
    return info;
}

bool ffmpeg::decodeImageSequence(const std::string &path, int maxFrames, int maxSize,
                                 size_t maxBytes, int *outWidth, int *outHeight, double *outFps,
                                 std::vector<unsigned char> *outPixels, std::string *error) {
    const auto fail = [&](const std::string &message) {
        if (error) *error = message;
        return false;
    };
    if (outPixels) outPixels->clear();
    MediaInfo info = probe(path);
    if (info.videoWidth <= 0 || info.videoHeight <= 0) {
        return fail(info.error.empty() ? "not a picture ffmpeg can read" : info.error);
    }
    // Scale to the cap before decoding, so ffmpeg never hands back a frame
    // bigger than the caller asked for (rawvideo is uncompressed).
    int width = info.videoWidth;
    int height = info.videoHeight;
    const int longest = std::max(width, height);
    if (maxSize > 0 && longest > maxSize) {
        const double scale = static_cast<double>(maxSize) / static_cast<double>(longest);
        width = std::max(1, static_cast<int>(std::lround(width * scale)));
        height =
            std::max(1, static_cast<int>(std::lround(static_cast<double>(height) * scale)));
    }
    const size_t frameBytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
    if (frameBytes == 0) return fail("the picture has no pixels");
    int frames = std::max(1, maxFrames);
    if (maxBytes > 0) {
        const int affordable = static_cast<int>(maxBytes / frameBytes);
        frames = std::max(1, std::min(frames, affordable));
    }
    char filter[96];
    std::snprintf(filter, sizeof(filter), "scale=%d:%d", width, height);
    std::vector<std::string> arguments = {
        ffmpegPath(),     "-hide_banner", "-v",           "error",   "-i",
        path,             "-vsync",       "0",            "-frames:v", std::to_string(frames),
        "-vf",            filter,         "-f",           "rawvideo", "-pix_fmt",
        "rgba",           "-"};
    ChildProcess process;
    if (!process.start(arguments, error)) return false;
    if (!process.wait(nullptr)) {
        const std::string message = process.stderrText();
        return fail(message.empty() ? "ffmpeg could not decode the picture" : message);
    }
    const std::string &raw = process.stdoutText();
    const size_t available = raw.size() / frameBytes;
    if (available == 0) return fail("ffmpeg returned no picture frames");
    const size_t used = std::min(available, static_cast<size_t>(frames));
    if (outPixels) {
        outPixels->assign(reinterpret_cast<const unsigned char *>(raw.data()),
                          reinterpret_cast<const unsigned char *>(raw.data()) +
                              used * frameBytes);
    }
    if (outWidth) *outWidth = width;
    if (outHeight) *outHeight = height;
    if (outFps) *outFps = info.videoFps;
    return true;
}

// ---------------------------------------------------------------------------
// ChildProcess
// ---------------------------------------------------------------------------

ChildProcess::~ChildProcess() {
    terminate();
    if (reader_.joinable()) reader_.join();
    if (stdoutReader_.joinable()) stdoutReader_.join();
#ifdef _WIN32
    if (handle_) {
        PROCESS_INFORMATION *info = static_cast<PROCESS_INFORMATION *>(handle_);
        if (info->hProcess) CloseHandle(info->hProcess);
        delete info;
        handle_ = nullptr;
    }
    if (stdoutRead_) {
        CloseHandle(static_cast<HANDLE>(stdoutRead_));
        stdoutRead_ = nullptr;
    }
#endif
}

bool ChildProcess::start(const std::vector<std::string> &arguments, std::string *error) {
    if (arguments.empty()) {
        if (error) *error = "empty command";
        return false;
    }
#ifdef _WIN32
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE stdinRead = nullptr, stdinWrite = nullptr;
    HANDLE stderrRead = nullptr, stderrWrite = nullptr;
    if (!CreatePipe(&stdinRead, &stdinWrite, &security, 1 << 20)) {
        if (error) *error = "CreatePipe failed";
        return false;
    }
    SetHandleInformation(stdinWrite, HANDLE_FLAG_INHERIT, 0);
    if (!CreatePipe(&stderrRead, &stderrWrite, &security, 1 << 16)) {
        CloseHandle(stdinRead);
        CloseHandle(stdinWrite);
        if (error) *error = "CreatePipe failed";
        return false;
    }
    SetHandleInformation(stderrRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = stdinRead;
    startup.hStdError = stderrWrite;
    // stdout is used for the actual data (decoded PCM / probe JSON), so it must
    // be captured as well.
    HANDLE stdoutRead = nullptr, stdoutWrite = nullptr;
    if (!CreatePipe(&stdoutRead, &stdoutWrite, &security, 1 << 24)) {
        CloseHandle(stdinRead);
        CloseHandle(stdinWrite);
        CloseHandle(stderrRead);
        CloseHandle(stderrWrite);
        if (error) *error = "CreatePipe failed";
        return false;
    }
    SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0);
    startup.hStdOutput = stdoutWrite;

    std::string commandLine = buildCommandLine(arguments);
    std::vector<char> mutableLine(commandLine.begin(), commandLine.end());
    mutableLine.push_back('\0');

    PROCESS_INFORMATION processInfo{};
    const BOOL created = CreateProcessA(nullptr, mutableLine.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                                        &processInfo);
    CloseHandle(stdinRead);
    CloseHandle(stderrWrite);
    CloseHandle(stdoutWrite);
    if (!created) {
        CloseHandle(stdinWrite);
        CloseHandle(stderrRead);
        CloseHandle(stdoutRead);
        if (error) {
            char buffer[128];
            std::snprintf(buffer, sizeof(buffer), "CreateProcess failed (error %lu)",
                          static_cast<unsigned long>(GetLastError()));
            *error = buffer;
        }
        return false;
    }
    CloseHandle(processInfo.hThread);

    stdinWrite_ = stdinWrite;
    stderrRead_ = stderrRead;
    stdoutRead_ = stdoutRead;
    handle_ = new PROCESS_INFORMATION(processInfo);
    running_ = true;
    stderrText_.clear();
    stdoutText_.clear();

    reader_ = std::thread([this]() {
        char buffer[4096];
        DWORD read = 0;
        while (ReadFile(static_cast<HANDLE>(stderrRead_), buffer, sizeof(buffer), &read, nullptr) &&
               read > 0) {
            std::lock_guard<std::mutex> lock(textMutex_);
            if (stderrText_.size() < 16384) {
                stderrText_.append(buffer, read);
            }
        }
    });
    stdoutReader_ = std::thread([this]() {
        char buffer[65536];
        DWORD read = 0;
        while (ReadFile(static_cast<HANDLE>(stdoutRead_), buffer, sizeof(buffer), &read, nullptr) &&
               read > 0) {
            std::lock_guard<std::mutex> lock(textMutex_);
            stdoutText_.append(buffer, read);
        }
    });
    return true;
#else
    (void)error;
    return false;
#endif
}

bool ChildProcess::write(const void *data, size_t size) {
    if (!running_ || !stdinWrite_) return false;
#ifdef _WIN32
    const unsigned char *bytes = static_cast<const unsigned char *>(data);
    size_t remaining = size;
    while (remaining > 0) {
        DWORD written = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(remaining, 1u << 20));
        if (!WriteFile(static_cast<HANDLE>(stdinWrite_), bytes, chunk, &written, nullptr) ||
            written == 0) {
            return false;
        }
        bytes += written;
        remaining -= written;
    }
    return true;
#else
    return false;
#endif
}

bool ChildProcess::closeStdin() {
    if (!stdinWrite_) return true;
#ifdef _WIN32
    CloseHandle(static_cast<HANDLE>(stdinWrite_));
#endif
    stdinWrite_ = nullptr;
    return true;
}

bool ChildProcess::wait(int *exitCode) {
    if (!handle_) return false;
    if (running_) closeStdin();
#ifdef _WIN32
    PROCESS_INFORMATION *info = static_cast<PROCESS_INFORMATION *>(handle_);
    WaitForSingleObject(info->hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(info->hProcess, &code);
    exitCode_ = static_cast<int>(code);
    running_ = false;
#endif
    if (reader_.joinable()) reader_.join();
    if (stdoutReader_.joinable()) stdoutReader_.join();
    if (exitCode) *exitCode = exitCode_;
    return exitCode_ == 0;
}

void ChildProcess::terminate() {
    if (!handle_) return;
    if (running_) {
#ifdef _WIN32
        PROCESS_INFORMATION *info = static_cast<PROCESS_INFORMATION *>(handle_);
        TerminateProcess(info->hProcess, 1);
        running_ = false;
#endif
    }
    closeStdin();
}

std::string ChildProcess::stdoutText() const {
    std::lock_guard<std::mutex> lock(textMutex_);
    return stdoutText_;
}

std::string ChildProcess::stderrText() const {
    std::lock_guard<std::mutex> lock(textMutex_);
    return stderrText_;
}

}  // namespace pf
