// Procedural roaster sound. One portable synthesizer (no OS code, no sample
// files) renders what a real roaster sounds like from the live simulation:
//
//   machine:  blower/fan motor and air rush, gas burner roar or element hum,
//             drum rumble, stir arm scraping, beans tumbling or rattling
//   cracks:   every first/second crack the physics produces, voiced from the
//             cracking bean's size, hardness and remaining moisture, so a dense
//             wet Kenya pops loud and deep and a decaf barely ticks
//
// Every front end plays the same sound: native shells pull it into their
// audio callback (or use platform::AudioOutput / rs_audio_play), and the web
// server streams it to browsers. See docs/SOUND.md.
#pragma once

#include <memory>

namespace roastsim {

class Host;

struct AudioMix {
    float master = 0.8f;
    float machine = 1.0f;  // fans, burner, drum, beans moving
    float cracks = 1.0f;   // first and second crack
};

class AudioSynth {
public:
    AudioSynth(Host& host, int sampleRate);
    ~AudioSynth();
    AudioSynth(const AudioSynth&) = delete;
    AudioSynth& operator=(const AudioSynth&) = delete;

    // Fills `frames` mono samples in [-1, 1]. Call from one thread at a time
    // (normally the audio callback); it reads the host thread-safely.
    void render(float* out, int frames);
    void setMix(const AudioMix& mix);
    AudioMix mix() const;
    int sampleRate() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace roastsim
