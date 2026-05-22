#include "decoder.h"
#include <cstdio>
#include <algorithm>
#include <vector>
#include <string>

#define DR_FLAC_IMPLEMENTATION
#include "../third_party/dr_flac.h"

#define DR_WAV_IMPLEMENTATION
#include "../third_party/dr_wav.h"

struct Decoder::Impl {
    drflac* flac = nullptr;
    drwav   wav  = {};
    bool    wavOpen = false;
};

Decoder::Decoder() : impl_(new Impl) {}

Decoder::~Decoder() {
    close();
    delete impl_;
}

static std::string fileExt(const std::string& path) {
    auto pos = path.rfind('.');
    if (pos == std::string::npos) return "";
    std::string ext = path.substr(pos);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext;
}

bool Decoder::open(const std::string& path) {
    close();
    std::string ext = fileExt(path);

    if (ext == ".wav") {
        if (!drwav_init_file(&impl_->wav, path.c_str(), nullptr)) return false;
        impl_->wavOpen = true;
        sampleRate_  = (int)impl_->wav.sampleRate;
        channels_    = (int)impl_->wav.channels;
        totalFrames_ = (int)impl_->wav.totalPCMFrameCount;
    } else {
        impl_->flac = drflac_open_file(path.c_str(), nullptr);
        if (!impl_->flac) return false;
        sampleRate_  = (int)impl_->flac->sampleRate;
        channels_    = (int)impl_->flac->channels;
        totalFrames_ = (int)impl_->flac->totalPCMFrameCount;
    }
    return true;
}

void Decoder::close() {
    stop();
    if (impl_->flac) {
        drflac_close(impl_->flac);
        impl_->flac = nullptr;
    }
    if (impl_->wavOpen) {
        drwav_uninit(&impl_->wav);
        impl_->wavOpen = false;
    }
    sampleRate_ = channels_ = totalFrames_ = 0;
}

void Decoder::startAsync(PcmCallback cb) {
    stop();
    running_.store(true);
    thread_ = std::thread([this, cb]{ decodeLoop(cb); });
}

void Decoder::stop() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
}

void Decoder::seekMs(int positionMs) {
    if (sampleRate_ == 0) return;
    seekTarget_.store(positionMs);
}

void Decoder::decodeLoop(PcmCallback cb) {
    static const int CHUNK_FRAMES = 4096;
    std::vector<float> buf(CHUNK_FRAMES * channels_);

    while (running_.load()) {
        int seekMs = seekTarget_.exchange(-1);
        if (seekMs >= 0) {
            uint64_t frame = (uint64_t)seekMs * sampleRate_ / 1000;
            if (impl_->flac)
                drflac_seek_to_pcm_frame(impl_->flac, frame);
            else if (impl_->wavOpen)
                drwav_seek_to_pcm_frame(&impl_->wav, frame);
        }

        drwav_uint64 got = 0;
        if (impl_->flac)
            got = drflac_read_pcm_frames_f32(impl_->flac, CHUNK_FRAMES, buf.data());
        else if (impl_->wavOpen)
            got = drwav_read_pcm_frames_f32(&impl_->wav, CHUNK_FRAMES, buf.data());

        if (got == 0) {
            running_.store(false);
            break;
        }

        cb(buf.data(), (int)(got * channels_));
    }
}
