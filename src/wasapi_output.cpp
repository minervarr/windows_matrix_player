#include "wasapi_output.h"
#include "log_util.h"
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <algorithm>
#include <cstring>
#include <cstdio>

// {00000003-0000-0010-8000-00AA00389B71}  IEEE_FLOAT
static const GUID kSubtypeFloat = {
    0x00000003, 0x0000, 0x0010,
    { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 }
};
// {00000001-0000-0010-8000-00AA00389B71}  PCM integer
static const GUID kSubtypePcm = {
    0x00000001, 0x0000, 0x0010,
    { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 }
};

static const char* wireFormatName(WireFormat wf) {
    switch (wf) {
    case WireFormat::Float32:  return "float32";
    case WireFormat::Int32:    return "int32";
    case WireFormat::Int24In32:return "int24in32";
    case WireFormat::Int16:    return "int16";
    }
    return "unknown";
}

// ── Device enumeration ────────────────────────────────────────────────────────

std::vector<WasapiDeviceInfo> WasapiOutput::enumerateDevices() {
    std::vector<WasapiDeviceInfo> out;
    IMMDeviceEnumerator* pEnum = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnum);
    if (FAILED(hr)) return out;

    IMMDeviceCollection* pCol = nullptr;
    pEnum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCol);
    pEnum->Release();
    if (!pCol) return out;

    UINT count = 0;
    pCol->GetCount(&count);

    for (UINT i = 0; i < count; i++) {
        IMMDevice* pDev = nullptr;
        if (FAILED(pCol->Item(i, &pDev))) continue;

        LPWSTR pwszId = nullptr;
        pDev->GetId(&pwszId);

        std::wstring name;
        IPropertyStore* pStore = nullptr;
        if (SUCCEEDED(pDev->OpenPropertyStore(STGM_READ, &pStore))) {
            PROPVARIANT var;
            PropVariantInit(&var);
            if (SUCCEEDED(pStore->GetValue(PKEY_Device_FriendlyName, &var)) &&
                var.vt == VT_LPWSTR)
                name = var.pwszVal;
            PropVariantClear(&var);
            pStore->Release();
        }

        if (pwszId) {
            out.push_back({ pwszId, name.empty() ? L"Unknown device" : name });
            CoTaskMemFree(pwszId);
        }
        pDev->Release();
    }
    pCol->Release();
    return out;
}

// ── Constructor / Destructor ──────────────────────────────────────────────────

WasapiOutput::WasapiOutput(std::wstring deviceId, WasapiMode mode)
    : deviceId_(std::move(deviceId)), mode_(mode)
{
    hDrainEvent_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
}

WasapiOutput::~WasapiOutput() {
    close();
    if (hDrainEvent_) { CloseHandle(hDrainEvent_); hDrainEvent_ = nullptr; }
}

// ── configure ─────────────────────────────────────────────────────────────────

bool WasapiOutput::configure(int rate, int channels, int /*bitDepth*/, bool strictBitperfect) {
    close();

    IMMDeviceEnumerator* pEnum = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnum);
    if (FAILED(hr)) {
        printf("[WASAPI] CoCreateInstance failed: 0x%08X\n", (unsigned)hr);
        return false;
    }

    hr = deviceId_.empty()
        ? pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice_)
        : pEnum->GetDevice(deviceId_.c_str(), &pDevice_);
    pEnum->Release();
    if (FAILED(hr)) {
        printf("[WASAPI] GetDevice failed: 0x%08X\n", (unsigned)hr);
        return false;
    }

    hr = pDevice_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&pAudioClient_);
    if (FAILED(hr)) {
        printf("[WASAPI] Activate failed: 0x%08X\n", (unsigned)hr);
        return false;
    }

    return (mode_ == WasapiMode::Exclusive)
        ? configureExclusive(rate, channels, strictBitperfect)
        : configureShared(channels);
}

// Build an EXTENSIBLE float32 WAVEFORMATEX for the given rate and channel count.
static WAVEFORMATEXTENSIBLE makeFloat32Format(int rate, int channels) {
    WAVEFORMATEXTENSIBLE f = {};
    f.Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
    f.Format.nChannels       = (WORD)channels;
    f.Format.nSamplesPerSec  = (DWORD)rate;
    f.Format.wBitsPerSample  = 32;
    f.Format.nBlockAlign     = (WORD)(channels * 4);
    f.Format.nAvgBytesPerSec = (DWORD)(rate * channels * 4);
    f.Format.cbSize          = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    f.Samples.wValidBitsPerSample = 32;
    f.dwChannelMask = 0;  // let the driver decide
    f.SubFormat     = kSubtypeFloat;
    return f;
}

static WAVEFORMATEXTENSIBLE makeExclusiveFormat(int rate, int channels, WireFormat wf) {
    WAVEFORMATEXTENSIBLE f = {};
    f.Format.wFormatTag     = WAVE_FORMAT_EXTENSIBLE;
    f.Format.nChannels      = (WORD)channels;
    f.Format.nSamplesPerSec = (DWORD)rate;
    f.Format.cbSize         = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);

    // Standard speaker masks; 0 for unusual channel counts.
    if      (channels == 1) f.dwChannelMask = SPEAKER_FRONT_CENTER;
    else if (channels == 2) f.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    else                    f.dwChannelMask = 0;

    switch (wf) {
    case WireFormat::Float32:
        f.Format.wBitsPerSample       = 32;
        f.Samples.wValidBitsPerSample = 32;
        f.SubFormat                   = kSubtypeFloat;
        break;
    case WireFormat::Int32:
        f.Format.wBitsPerSample       = 32;
        f.Samples.wValidBitsPerSample = 32;
        f.SubFormat                   = kSubtypePcm;
        break;
    case WireFormat::Int24In32:
        f.Format.wBitsPerSample       = 32;
        f.Samples.wValidBitsPerSample = 24;
        f.SubFormat                   = kSubtypePcm;
        break;
    case WireFormat::Int16:
        f.Format.wBitsPerSample       = 16;
        f.Samples.wValidBitsPerSample = 16;
        f.SubFormat                   = kSubtypePcm;
        break;
    }
    f.Format.nBlockAlign     = (WORD)(channels * (f.Format.wBitsPerSample / 8));
    f.Format.nAvgBytesPerSec = (DWORD)(rate * f.Format.nBlockAlign);
    return f;
}

bool WasapiOutput::configureShared(int srcChannels) {
    useEvent_ = true;
    WAVEFORMATEX* pMix = nullptr;
    HRESULT hr = pAudioClient_->GetMixFormat(&pMix);
    if (FAILED(hr)) return false;

    int mixRate = (int)pMix->nSamplesPerSec;
    int mixCh   = (int)pMix->nChannels;
    CoTaskMemFree(pMix);

    rate_     = mixRate;
    channels_ = mixCh;

    WAVEFORMATEXTENSIBLE wfex = makeFloat32Format(mixRate, mixCh);

    WAVEFORMATEX* pClosest = nullptr;
    hr = pAudioClient_->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED,
                                          (WAVEFORMATEX*)&wfex, &pClosest);
    if (pClosest) CoTaskMemFree(pClosest);
    if (FAILED(hr) && hr != S_FALSE) {
        printf("[WASAPI] Shared float32 not supported: 0x%08X\n", (unsigned)hr);
        return false;
    }

    // 200 ms buffer
    REFERENCE_TIME bufDur = 2000000LL;
    hr = pAudioClient_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                   AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                   bufDur, 0, (WAVEFORMATEX*)&wfex, nullptr);
    if (FAILED(hr)) {
        printf("[WASAPI] Initialize(shared) failed: 0x%08X\n", (unsigned)hr);
        return false;
    }

    pAudioClient_->GetBufferSize(&bufferFrames_);
    hEvent_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    pAudioClient_->SetEventHandle(hEvent_);

    hr = pAudioClient_->GetService(__uuidof(IAudioRenderClient), (void**)&pRenderClient_);
    if (FAILED(hr)) return false;

    allocRing();
    printf("[%s][WASAPI] Shared: %d Hz %d ch float32, buffer=%u frames\n",
           logTs(), rate_, channels_, bufferFrames_);
    return true;
}

static HRESULT tryExclusiveInit(IAudioClient* client, DWORD flags,
                                REFERENCE_TIME period, WAVEFORMATEXTENSIBLE& wfex,
                                IMMDevice* device, IAudioClient** outClient, int rate) {
    HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, flags,
                                    period, period, (WAVEFORMATEX*)&wfex, nullptr);
    if (hr == (HRESULT)0x88890019) {
        UINT32 aligned = 0;
        client->GetBufferSize(&aligned);
        client->Release();
        *outClient = nullptr;
        if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                    nullptr, (void**)outClient)))
            return hr;
        REFERENCE_TIME alignedDur = (REFERENCE_TIME)((10000000.0 * aligned / rate) + 0.5);
        hr = (*outClient)->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, flags,
                                      alignedDur, alignedDur, (WAVEFORMATEX*)&wfex, nullptr);
    }
    return hr;
}

bool WasapiOutput::configureExclusive(int rate, int channels, bool strictBitperfect) {
    static const WireFormat kFormats[] = {
        WireFormat::Float32, WireFormat::Int32, WireFormat::Int24In32, WireFormat::Int16
    };

    REFERENCE_TIME def = 0, minPeriod = 0;
    pAudioClient_->GetDevicePeriod(&def, &minPeriod);

    for (WireFormat wf : kFormats) {
        if (!pAudioClient_) {
            if (FAILED(pDevice_->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                          nullptr, (void**)&pAudioClient_)))
                break;
            pAudioClient_->GetDevicePeriod(&def, &minPeriod);
        }

        WAVEFORMATEXTENSIBLE wfex = makeExclusiveFormat(rate, channels, wf);

        HRESULT hr = pAudioClient_->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                                      (WAVEFORMATEX*)&wfex, nullptr);
        if (FAILED(hr)) {
            printf("[WASAPI] Exclusive %s %d Hz %dch: IsFormatSupported failed 0x%08X\n",
                   wireFormatName(wf), rate, channels, (unsigned)hr);
            continue;
        }

        // Try default period first (safer scheduling headroom), then min period
        REFERENCE_TIME periods[] = { def, minPeriod };
        bool opened = false;

        for (REFERENCE_TIME tryPeriod : periods) {
            if (!pAudioClient_) {
                if (FAILED(pDevice_->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                              nullptr, (void**)&pAudioClient_)))
                    break;
            }

            // Event-driven attempt
            hr = tryExclusiveInit(pAudioClient_, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                  tryPeriod, wfex, pDevice_, &pAudioClient_, rate);
            if (SUCCEEDED(hr)) {
                useEvent_ = true;
                opened = true;
                break;
            }

            printf("[WASAPI] Exclusive %s event-mode period=%lld failed (0x%08X)\n",
                   wireFormatName(wf), tryPeriod / 10000LL, (unsigned)hr);
            if (pAudioClient_) { pAudioClient_->Release(); pAudioClient_ = nullptr; }

            // Polling fallback
            if (FAILED(pDevice_->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                          nullptr, (void**)&pAudioClient_)))
                break;

            hr = tryExclusiveInit(pAudioClient_, 0,
                                  tryPeriod, wfex, pDevice_, &pAudioClient_, rate);
            if (SUCCEEDED(hr)) {
                useEvent_ = false;
                opened = true;
                break;
            }

            printf("[WASAPI] Exclusive %s polling period=%lld also failed (0x%08X)\n",
                   wireFormatName(wf), tryPeriod / 10000LL, (unsigned)hr);
            if (pAudioClient_) { pAudioClient_->Release(); pAudioClient_ = nullptr; }
        }

        if (!opened) continue;

        wireFormat_ = wf;
        rate_       = rate;
        channels_   = channels;
        pAudioClient_->GetBufferSize(&bufferFrames_);

        REFERENCE_TIME actualDef = 0, actualMin = 0;
        pAudioClient_->GetDevicePeriod(&actualDef, &actualMin);
        periodMs_ = (int)(actualDef / 10000LL);
        if (periodMs_ < 2) periodMs_ = 2;

        if (useEvent_) {
            hEvent_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
            pAudioClient_->SetEventHandle(hEvent_);
        }

        pAudioClient_->GetService(__uuidof(IAudioRenderClient), (void**)&pRenderClient_);
        allocRing();
        printf("[%s][WASAPI] Exclusive: %d Hz %d ch %s, buffer=%u frames, period=%d ms, %s\n",
               logTs(), rate_, channels_, wireFormatName(wf), bufferFrames_, periodMs_,
               useEvent_ ? "event-driven" : "polling");
        return true;
    }

    if (strictBitperfect) {
        printf("[WASAPI] Exclusive: no supported format for %d Hz %dch, strict bitperfect failing\n",
               rate, channels);
        return false;
    }

    // No exclusive format worked — fall back to shared mode so devices like Bluetooth
    // that don't support exclusive still play. The caller reads getConfiguredRate() and
    // resamples if the OS mix rate differs from the file rate.
    printf("[WASAPI] Exclusive: no supported format for %d Hz %dch, falling back to shared\n",
           rate, channels);
    if (!pAudioClient_)
        pDevice_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&pAudioClient_);
    mode_ = WasapiMode::Shared;
    useEvent_ = true;
    return configureShared(channels);
}

// ── Format probing ─────────────────────────────────────────────────────────────

static IAudioClient* activateClient(IMMDevice* pDevice) {
    IAudioClient* pClient = nullptr;
    pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&pClient);
    return pClient;
}

std::vector<int> WasapiOutput::probeRates(int channels) const {
    static const int kRates[] = {
        44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000
    };
    std::vector<int> supported;
    if (!pDevice_) return supported;

    IAudioClient* pClient = activateClient(pDevice_);
    if (!pClient) return supported;

    for (int rate : kRates) {
        WAVEFORMATEXTENSIBLE wfex = makeExclusiveFormat(rate, channels, WireFormat::Int32);
        HRESULT hr = pClient->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                                (WAVEFORMATEX*)&wfex, nullptr);
        if (SUCCEEDED(hr))
            supported.push_back(rate);
    }
    pClient->Release();
    return supported;
}

int WasapiOutput::getMaxBitDepth(int rate, int channels) const {
    if (!pDevice_) return 32;
    IAudioClient* pClient = activateClient(pDevice_);
    if (!pClient) return 32;

    static const WireFormat kOrder[] = {
        WireFormat::Int32, WireFormat::Int24In32, WireFormat::Int16
    };
    static const int kBits[] = { 32, 24, 16 };

    int result = 32;
    for (int i = 0; i < 3; i++) {
        WAVEFORMATEXTENSIBLE wfex = makeExclusiveFormat(rate, channels, kOrder[i]);
        HRESULT hr = pClient->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                                (WAVEFORMATEX*)&wfex, nullptr);
        if (SUCCEEDED(hr)) { result = kBits[i]; break; }
    }
    pClient->Release();
    printf("[WASAPI] getMaxBitDepth(%d Hz, %dch) = %d\n", rate, channels, result);
    return result;
}

// ── Ring buffer ────────────────────────────────────────────────────────────────

void WasapiOutput::allocRing() {
    size_t floats;
    if (mode_ == WasapiMode::Exclusive) {
        // Small ring limits gapless audio lag to ~200 ms (16 × hardware period).
        // Prevents old track's tail bleeding audibly into the next track.
        floats = (size_t)bufferFrames_ * channels_ * 16;
        if (floats < 4096) floats = 4096;
    } else {
        // Shared: WASAPI hardware buffer is 200 ms; ring ~125 ms stays ahead safely.
        floats = (size_t)(rate_ * channels_) / 8;
        if (floats < 4096) floats = 4096;
    }
    size_t cap = 1;
    while (cap < floats) cap <<= 1;
    ring_.assign(cap, 0.0f);
    ringMask_ = cap - 1;
    readPos_.store(0, std::memory_order_relaxed);
    writePos_.store(0, std::memory_order_relaxed);
    printf("[%s][WASAPI] ring: %zu floats (%.0f ms)\n",
           logTs(), cap, (double)cap / (rate_ * channels_) * 1000.0);
    fflush(stdout);
}

int WasapiOutput::writeRing(const float* src, int n) {
    size_t wp   = writePos_.load(std::memory_order_relaxed);
    size_t rp   = readPos_.load(std::memory_order_acquire);
    size_t cap  = ringMask_ + 1;
    size_t free = cap - (wp - rp);
    int w = (int)std::min((size_t)n, free);
    for (int i = 0; i < w; i++) ring_[(wp + i) & ringMask_] = src[i];
    writePos_.store(wp + w, std::memory_order_release);
    return w;
}

int WasapiOutput::readRing(float* dst, int n) {
    size_t rp    = readPos_.load(std::memory_order_relaxed);
    size_t wp    = writePos_.load(std::memory_order_acquire);
    size_t avail = wp - rp;
    int r = (int)std::min((size_t)n, avail);
    for (int i = 0; i < r; i++) dst[i] = ring_[(rp + i) & ringMask_];
    readPos_.store(rp + r, std::memory_order_release);
    return r;
}

// ── start / stop / flush / close ─────────────────────────────────────────────

bool WasapiOutput::start() {
    if (!pAudioClient_ || !pRenderClient_) return false;

    // Pre-fill hardware buffer so the driver has valid audio for its first period
    BYTE* pData = nullptr;
    HRESULT hr = pRenderClient_->GetBuffer(bufferFrames_, &pData);
    if (SUCCEEDED(hr)) {
        int need = (int)bufferFrames_ * channels_;
        if (wireFormat_ == WireFormat::Float32) {
            int got = readRing(reinterpret_cast<float*>(pData), need);
            if (got < need)
                memset(reinterpret_cast<float*>(pData) + got, 0,
                       (size_t)(need - got) * sizeof(float));
            pRenderClient_->ReleaseBuffer(bufferFrames_, got == 0 ? AUDCLNT_BUFFERFLAGS_SILENT : 0);
        } else {
            std::vector<float> staging(need);
            int got = readRing(staging.data(), need);
            if (got < need)
                memset(staging.data() + got, 0, (size_t)(need - got) * sizeof(float));
            if (got == 0) {
                memset(pData, 0, (size_t)bufferFrames_ * channels_ * (wireFormat_ == WireFormat::Int16 ? 2 : 4));
                pRenderClient_->ReleaseBuffer(bufferFrames_, AUDCLNT_BUFFERFLAGS_SILENT);
            } else {
                convertToWire(staging.data(), pData, need);
                pRenderClient_->ReleaseBuffer(bufferFrames_, 0);
            }
        }
    }

    printf("[%s][WASAPI] start(): pre-fill=%d ring_avail=%zu\n",
           logTs(), (int)bufferFrames_ * channels_, ringAvailable());
    fflush(stdout);

    running_.store(true, std::memory_order_release);
    started_.store(true, std::memory_order_release);
    renderThread_ = std::thread(&WasapiOutput::renderLoop, this);
    hr = pAudioClient_->Start();
    if (FAILED(hr)) {
        running_.store(false);
        if (renderThread_.joinable()) renderThread_.join();
        printf("[%s][WASAPI] Start failed: 0x%08X\n", logTs(), (unsigned)hr);
        return false;
    }
    return true;
}

void WasapiOutput::stop() {
    printf("[%s][WASAPI] stop(): ring_avail=%zu\n", logTs(), ringAvailable());
    fflush(stdout);
    running_.store(false, std::memory_order_release);
    if (hEvent_) SetEvent(hEvent_);
    if (hDrainEvent_) SetEvent(hDrainEvent_);
    if (renderThread_.joinable()) renderThread_.join();
    if (pAudioClient_) pAudioClient_->Stop();
    readPos_.store(0, std::memory_order_relaxed);
    writePos_.store(0, std::memory_order_relaxed);
    underrunCount_.store(0, std::memory_order_relaxed);
}

void WasapiOutput::flush() {
    // Discard buffered samples (e.g. after seek)
    readPos_.store(writePos_.load(std::memory_order_acquire), std::memory_order_release);
}

void WasapiOutput::close() {
    stop();
    started_.store(false, std::memory_order_release);
    if (pRenderClient_) { pRenderClient_->Release(); pRenderClient_ = nullptr; }
    if (pAudioClient_)  { pAudioClient_->Release();  pAudioClient_  = nullptr; }
    if (pDevice_)       { pDevice_->Release();        pDevice_       = nullptr; }
    if (hEvent_)        { CloseHandle(hEvent_);       hEvent_        = nullptr; }
    ring_.clear();
    ringMask_ = 0;
    rate_ = channels_ = 0;
}

// ── writeFloat32 ─────────────────────────────────────────────────────────────

int WasapiOutput::writeFloat32(const float* data, int numSamples) {
    return writeRing(data, numSamples);
}

int WasapiOutput::writeFloat32Blocking(const float* data, int numSamples, int timeoutMs) {
    int total = 0;
    DWORD start = GetTickCount();
    while (total < numSamples) {
        int written = writeRing(data + total, numSamples - total);
        total += written;
        if (total >= numSamples) break;
        if (started_.load(std::memory_order_acquire) &&
            !running_.load(std::memory_order_acquire)) break;
        DWORD elapsed = GetTickCount() - start;
        if ((int)elapsed >= timeoutMs) {
            printf("[%s][WASAPI] writeFloat32Blocking timeout after %d ms (%d/%d written)\n",
                   logTs(), timeoutMs, total, numSamples);
            fflush(stdout);
            break;
        }
        WaitForSingleObject(hDrainEvent_, 5);
    }
    return total;
}

int WasapiOutput::writeInt32Blocking(const int32_t* data, int numSamples, int timeoutMs) {
    std::vector<float> tmp(numSamples);
    for (int i = 0; i < numSamples; ++i)
        tmp[i] = (float)((double)data[i] / 2147483648.0);
    return writeFloat32Blocking(tmp.data(), numSamples, timeoutMs);
}

int WasapiOutput::getPreBufferSamples() const {
    if (!bufferFrames_) return 4096;
    if (mode_ == WasapiMode::Exclusive)
        return (int)bufferFrames_ * channels_ * 2;   // 2 hardware periods
    else
        return (int)bufferFrames_ * channels_ / 2;   // half of shared buffer
}

size_t WasapiOutput::ringAvailable() const {
    return writePos_.load(std::memory_order_acquire)
         - readPos_.load(std::memory_order_acquire);
}

bool WasapiOutput::waitForData(int minSamples, int timeoutMs) {
    DWORD start = GetTickCount();
    while ((int)ringAvailable() < minSamples) {
        DWORD elapsed = GetTickCount() - start;
        if ((int)elapsed >= timeoutMs) return false;
        Sleep(2);
    }
    return true;
}

// ── Wire format conversion ────────────────────────────────────────────────────

void WasapiOutput::convertToWire(const float* src, BYTE* dst, int n) {
    switch (wireFormat_) {
    case WireFormat::Int32: {
        auto* out = reinterpret_cast<int32_t*>(dst);
        for (int i = 0; i < n; i++) {
            double d = std::max(-1.0, std::min(1.0, (double)src[i]));
            out[i] = (int32_t)(d * 2147483647.0);
        }
        break;
    }
    case WireFormat::Int24In32: {
        auto* out = reinterpret_cast<int32_t*>(dst);
        for (int i = 0; i < n; i++) {
            double d = std::max(-1.0, std::min(1.0, (double)src[i]));
            out[i] = (int32_t)(d * 8388607.0) << 8;
        }
        break;
    }
    case WireFormat::Int16: {
        auto* out = reinterpret_cast<int16_t*>(dst);
        for (int i = 0; i < n; i++) {
            double d = std::max(-1.0, std::min(1.0, (double)src[i]));
            out[i] = (int16_t)(d * 32767.0);
        }
        break;
    }
    default: break;
    }
}

// ── Render thread ─────────────────────────────────────────────────────────────

void WasapiOutput::renderLoop() {
    DWORD taskIndex = 0;
    HANDLE hTask = AvSetMmThreadCharacteristics(L"Pro Audio", &taskIndex);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    std::vector<float> staging;

    while (running_.load(std::memory_order_acquire)) {
        if (useEvent_) {
            DWORD wait = WaitForSingleObject(hEvent_, 200);
            if (wait != WAIT_OBJECT_0) continue;
        } else {
            Sleep(periodMs_ >= 4 ? periodMs_ / 2 : 2);
        }
        if (!running_.load(std::memory_order_acquire)) break;

        UINT32 padding = 0;
        if (mode_ == WasapiMode::Shared)
            pAudioClient_->GetCurrentPadding(&padding);

        UINT32 toWrite = bufferFrames_ - padding;
        if (toWrite == 0) continue;

        BYTE* pData = nullptr;
        if (FAILED(pRenderClient_->GetBuffer(toWrite, &pData))) continue;

        int need = (int)toWrite * channels_;
        int got;

        if (wireFormat_ == WireFormat::Float32) {
            got = readRing(reinterpret_cast<float*>(pData), need);
            if (got < need)
                memset(reinterpret_cast<float*>(pData) + got, 0,
                       (size_t)(need - got) * sizeof(float));
        } else {
            staging.resize(need);
            got = readRing(staging.data(), need);
            if (got < need)
                memset(staging.data() + got, 0, (size_t)(need - got) * sizeof(float));
            convertToWire(staging.data(), pData, need);
        }

        pRenderClient_->ReleaseBuffer(toWrite, 0);

        if (hDrainEvent_) SetEvent(hDrainEvent_);

        if (got < need) {
            int count = underrunCount_.fetch_add(1, std::memory_order_relaxed) + 1;
            if (count <= 5 || (count % 50) == 0)
                printf("[%s][WASAPI] underrun #%d: needed %d samples, got %d\n",
                       logTs(), count, need, got);
        }
    }

    if (hTask) AvRevertMmThreadCharacteristics(hTask);
}
