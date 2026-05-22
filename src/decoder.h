#pragma once
#include <string>
#include <functional>
#include <atomic>
#include <thread>

// TODO(FFmpeg): Swap this interface implementation for FFmpeg when formats beyond
// FLAC are needed (DSF/DFF containers, MP3, WAV, etc.). The interface stays the same.

// Called from the decode thread with interleaved float32 PCM samples.
// numSamples = total samples across all channels (frames * channels).
using PcmCallback = std::function<void(const float* data, int numSamples)>;

class Decoder {
public:
    Decoder();
    ~Decoder();

    bool open(const std::string& path);
    void close();

    int  sampleRate()  const { return sampleRate_; }
    int  channels()    const { return channels_; }
    int  totalFrames() const { return totalFrames_; }

    // Start decode loop on a background thread, calling cb with PCM chunks.
    void startAsync(PcmCallback cb);
    void stop();

    // Seek to position in milliseconds. Thread-safe.
    void seekMs(int positionMs);

    bool isRunning() const { return running_.load(); }

private:
    void decodeLoop(PcmCallback cb);

    struct Impl;
    Impl* impl_ = nullptr;

    int sampleRate_  = 0;
    int channels_    = 0;
    int totalFrames_ = 0;

    std::thread       thread_;
    std::atomic<bool> running_{false};
    std::atomic<int>  seekTarget_{-1}; // -1 = no pending seek
};
