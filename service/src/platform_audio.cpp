// Audio output backend. Windows: waveOut with a small ring of buffers refilled
// on a dedicated thread (~80 ms latency). Other platforms: not implemented yet.
#include <atomic>
#include <thread>
#include <vector>

#include "roastsim/platform.hpp"

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#endif

namespace roastsim::platform {

#ifdef _WIN32

struct AudioOutput::Impl {
    HWAVEOUT wave = nullptr;
    HANDLE event = nullptr;
    std::thread thread;
    std::atomic<bool> stop{false};
    std::atomic<bool> running{false};
    Fill fill = nullptr;
    void* user = nullptr;
    int frames = 0;
    std::vector<std::vector<short>> pcm;
    std::vector<WAVEHDR> hdr;
    std::vector<float> scratch;

    void refill(int i) {
        fill(user, scratch.data(), frames);
        for (int k = 0; k < frames; ++k) {
            float v = scratch[static_cast<std::size_t>(k)];
            v = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
            pcm[static_cast<std::size_t>(i)][static_cast<std::size_t>(k)] = static_cast<short>(v * 32767.0f);
        }
        WAVEHDR& h = hdr[static_cast<std::size_t>(i)];
        h.dwFlags &= ~static_cast<DWORD>(WHDR_DONE);
        waveOutWrite(wave, &h, sizeof h);
    }

    void loop() {
        while (!stop) {
            WaitForSingleObject(event, 50);
            for (std::size_t i = 0; i < hdr.size() && !stop; ++i)
                if (hdr[i].dwFlags & WHDR_DONE) refill(static_cast<int>(i));
        }
    }
};

AudioOutput::AudioOutput() : impl_(new Impl) {}
AudioOutput::~AudioOutput() {
    stop();
    delete impl_;
}

bool AudioOutput::start(int sampleRate, Fill fill, void* user, std::string* err) {
    stop();
    Impl& d = *impl_;
    d.fill = fill;
    d.user = user;
    d.frames = sampleRate / 50;  // 20 ms per buffer, 4 buffers
    d.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 1;
    fmt.nSamplesPerSec = static_cast<DWORD>(sampleRate);
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 2;
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * 2;
    if (waveOutOpen(&d.wave, WAVE_MAPPER, &fmt, reinterpret_cast<DWORD_PTR>(d.event), 0, CALLBACK_EVENT) !=
        MMSYSERR_NOERROR) {
        if (err) *err = "no audio output device";
        CloseHandle(d.event);
        d.event = nullptr;
        d.wave = nullptr;
        return false;
    }
    d.pcm.assign(4, std::vector<short>(static_cast<std::size_t>(d.frames)));
    d.hdr.assign(4, WAVEHDR{});
    d.scratch.assign(static_cast<std::size_t>(d.frames), 0.0f);
    for (std::size_t i = 0; i < d.hdr.size(); ++i) {
        d.hdr[i].lpData = reinterpret_cast<LPSTR>(d.pcm[i].data());
        d.hdr[i].dwBufferLength = static_cast<DWORD>(d.frames * 2);
        waveOutPrepareHeader(d.wave, &d.hdr[i], sizeof(WAVEHDR));
        d.refill(static_cast<int>(i));
    }
    d.stop = false;
    d.running = true;
    d.thread = std::thread([&d] { d.loop(); });
    return true;
}

void AudioOutput::stop() {
    Impl& d = *impl_;
    if (!d.running) return;
    d.stop = true;
    if (d.thread.joinable()) d.thread.join();
    waveOutReset(d.wave);
    for (auto& h : d.hdr) waveOutUnprepareHeader(d.wave, &h, sizeof(WAVEHDR));
    waveOutClose(d.wave);
    CloseHandle(d.event);
    d.wave = nullptr;
    d.event = nullptr;
    d.running = false;
}

bool AudioOutput::running() const { return impl_->running; }

#else  // not yet: ALSA/PulseAudio (Linux) and BSoundPlayer (Haiku) slot in here

struct AudioOutput::Impl {};
AudioOutput::AudioOutput() : impl_(new Impl) {}
AudioOutput::~AudioOutput() { delete impl_; }
bool AudioOutput::start(int, Fill, void*, std::string* err) {
    if (err) *err = "local audio output is not implemented on this platform yet; use the web UI's sound";
    return false;
}
void AudioOutput::stop() {}
bool AudioOutput::running() const { return false; }

#endif

}  // namespace roastsim::platform
