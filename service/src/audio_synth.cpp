// Procedural roaster sound synthesizer (see audio.hpp and docs/SOUND.md).
//
// Everything is generated from noise and oscillators; no samples. Parameters
// are pulled from the Host ~40 times a second and smoothed per sample, so the
// sound follows the controls (fan up -> higher whine, gas up -> louder roar)
// and the beans (tumble gets brighter and lighter as they roast).
#include "roastsim/audio.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <vector>

#include "roastsim/host.hpp"

namespace roastsim {

namespace {

constexpr float kTwoPi = 6.28318530718f;

struct Noise {
    std::uint32_t s = 0x9E3779B9u;
    float white() {  // uniform in [-1, 1)
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return static_cast<float>(s >> 8) * (1.0f / 8388608.0f) - 1.0f;
    }
    float unit() { return 0.5f * (white() + 1.0f); }  // [0, 1)
};

// RBJ cookbook biquad, transposed direct form II.
struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void set(float fs, float f, float q, int type) {  // 0 lowpass, 1 bandpass, 2 highpass
        f = std::min(std::max(f, 20.0f), 0.45f * fs);
        float w = kTwoPi * f / fs, c = std::cos(w), alpha = std::sin(w) / (2.0f * q);
        float a0 = 1.0f + alpha;
        if (type == 0) { b0 = (1 - c) / 2; b1 = 1 - c; b2 = (1 - c) / 2; }
        else if (type == 1) { b0 = alpha; b1 = 0; b2 = -alpha; }
        else { b0 = (1 + c) / 2; b1 = -(1 + c); b2 = (1 + c) / 2; }
        a1 = -2.0f * c;
        a2 = 1.0f - alpha;
        b0 /= a0; b1 /= a0; b2 /= a0; a1 /= a0; a2 /= a0;
    }
    float run(float x) {
        float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

// Smoothed parameter: glides toward its target over ~50 ms.
struct Param {
    float v = 0, target = 0;
    void tick(float k) { v += (target - v) * k; }
};

// A short percussive sound: decaying noise through a resonant band-pass, plus
// an optional decaying sine "thump". Used for bean impacts and cracks.
struct Voice {
    bool on = false;
    int delay = 0;
    float env = 0, decay = 0, amp = 0;
    Biquad bp;
    // recursive sine oscillator for the thump
    float y1 = 0, y2 = 0, k = 0, toneEnv = 0, toneDecay = 0;
    bool crack = false;

    float tick(Noise& n) {
        if (delay > 0) {
            --delay;
            return 0.0f;
        }
        float x = bp.run(n.white() * env) * amp;
        env *= decay;
        if (toneEnv > 1e-5f) {
            float y = k * y1 - y2;
            y2 = y1;
            y1 = y;
            x += y * toneEnv;
            toneEnv *= toneDecay;
        }
        if (env < 1e-4f && toneEnv < 1e-5f) on = false;
        return x;
    }
};

float softClip(float x) {  // smooth limiter, ~linear below 0.5
    float a = std::fabs(x);
    if (a < 0.5f) return x;
    float y = 0.5f + (a - 0.5f) / (1.0f + (a - 0.5f) * 2.0f);
    return x < 0 ? -y : y;
}

}  // namespace

struct AudioSynth::Impl {
    Host& host;
    float fs;
    mutable std::mutex mixMu;
    AudioMix mix;
    Noise noise;

    // Machine description (refreshed when the roaster changes)
    std::string roasterId;
    RoasterDef def;
    RoasterKind kind = RoasterKind::FluidBed;
    bool popper = false, gas = false;
    float heatMax = 1000, flowMax = 0.006f, rpmMax = 60, stirMax = 12, mains = 60;
    float hardness = 1.0f, size0 = 7.5f;

    // Layers
    Param airLevel, airFc, motorLevel, motorF, motorBright, burnerLevel, humLevel, rumbleLevel, rumbleHz,
        scrapeLevel, scrapeHz, grainRate, grainF, grainAmp, grainMod;
    Biquad airBp, burnerLp, rumbleLp, scrapeBp;
    float motorPh = 0, humPh = 0, rumblePh = 0, scrapePh = 0, flutterPh = 0, brown = 0;
    float grainTau = 0.005f;
    float crackMuffle = 1.0f, crackLp = 1.0f;

    std::vector<Voice> voices;
    std::uint64_t lastCrack = 0;
    std::uint32_t lastTeleAlarms = 0;
    int untilRefresh = 0;
    float smoothK = 0;

    Impl(Host& h, int rate) : host(h), fs(static_cast<float>(rate)) {
        voices.resize(128);
        smoothK = 1.0f - std::exp(-1.0f / (0.05f * fs));
        lastCrack = host.crackSeq();
        noise.s ^= static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(this));
    }

    Voice* freeVoice(bool crack) {
        for (auto& v : voices)
            if (!v.on) return &v;
        // Steal the quietest non-crack voice (or any, for a crack).
        Voice* best = nullptr;
        for (auto& v : voices)
            if ((crack || !v.crack) && (!best || v.env * v.amp < best->env * best->amp)) best = &v;
        return best;
    }

    void spawn(float f, float q, float tau, float amp, int delay, bool crack, float thumpF = 0, float thumpAmp = 0,
               float thumpTau = 0.015f) {
        Voice* v = freeVoice(crack);
        if (!v) return;
        v->on = true;
        v->crack = crack;
        v->delay = delay;
        v->env = 1.0f;
        v->decay = std::exp(-1.0f / (std::max(0.0005f, tau) * fs));
        // Narrow band-passes pass less noise power; compensate so `amp` ~ peak level.
        v->amp = amp * std::sqrt(q) * 1.6f;
        v->bp = Biquad{};
        v->bp.set(fs, f, q, 1);
        v->toneEnv = 0;
        if (thumpAmp > 0 && thumpF > 0) {
            float w = kTwoPi * thumpF / fs;
            v->k = 2.0f * std::cos(w);
            v->y1 = std::sin(w);
            v->y2 = 0.0f;
            v->toneEnv = thumpAmp;
            v->toneDecay = std::exp(-1.0f / (thumpTau * fs));
        }
    }

    void loadMachine(const Snapshot& s) {
        roasterId = s.roaster_id;
        def = host.roasterDef();
        BeanSpec spec = host.beanSpec();
        kind = def.kind;
        hardness = static_cast<float>(spec.density > 0 ? spec.density / spec.kinetics.density_ref : 1.0);
        size0 = static_cast<float>(std::cbrt(6.0 * spec.bean_mass_mg * 1e-6 / (kPi * spec.kinetics.density_ref)) * 1000.0);
        gas = false;
        switch (kind) {
            case RoasterKind::FluidBed:
                heatMax = static_cast<float>(def.fluid.heater_W);
                flowMax = static_cast<float>(def.fluid.fan_max_flow);
                popper = def.fluid.wiring == "stock" || def.fluid.chamber_diameter_m < 0.09;
                mains = def.fluid.nominal_V >= 200 ? 50.0f : 60.0f;
                break;
            case RoasterKind::Drum:
                heatMax = static_cast<float>(def.drum.heat_max_W);
                flowMax = static_cast<float>(def.drum.airflow_max);
                rpmMax = static_cast<float>(def.drum.rpm_max);
                gas = def.drum.heat_source == "gas";
                mains = def.drum.nominal_V >= 200 ? 50.0f : 60.0f;
                break;
            case RoasterKind::StirPan:
                heatMax = static_cast<float>(def.pan.heater_W);
                flowMax = static_cast<float>(def.pan.fan_max_flow);
                stirMax = static_cast<float>(def.pan.stir_rpm_max);
                mains = def.pan.nominal_V >= 200 ? 50.0f : 60.0f;
                break;
        }
    }

    // Pull the simulation state and turn it into layer targets.
    void refresh() {
        Snapshot s = host.snapshot();
        if (s.roaster_id != roasterId) loadMachine(s);
        // Bean identity can change between roasts with the same roaster.
        if (s.beans.present) {
            BeanSpec spec = host.beanSpec();
            hardness = static_cast<float>(spec.density > 0 ? spec.density / spec.kinetics.density_ref : 1.0);
            size0 = static_cast<float>(std::cbrt(6.0 * spec.bean_mass_mg * 1e-6 / (kPi * spec.kinetics.density_ref)) * 1000.0);
        }
        const MachineTelemetry& t = s.tele;
        float heat = clamp01f(static_cast<float>(t.heater_W) / std::max(1.0f, heatMax));
        float fan = clamp01f(static_cast<float>(t.airflow_kg_s) / std::max(1e-6f, flowMax));
        if (kind == RoasterKind::StirPan) fan = static_cast<float>(s.actuators[static_cast<int>(Control::Fan)] / 100.0);
        bool inMachine = s.bean_location == BeanLocation::Machine;
        bool inTray = s.bean_location == BeanLocation::Tray;
        float massNorm = s.beans.present ? static_cast<float>(s.beans.mass_g / std::max(1.0, def.batch.default_g)) : 0.0f;
        float size = static_cast<float>(size0 * (s.beans.present ? s.beans.expansion : 1.0));
        // Green, wet beans thud; dry roasted beans are light and rattle brightly.
        float roast = clamp01f(static_cast<float>(s.beans.dev_mean / 350.0));
        float wet = clamp01f(static_cast<float>((s.beans.moisture_pct - 3.0) / 8.0));
        float bright = 0.8f + 0.8f * roast - 0.25f * wet;

        // Reset everything, then let the family recipe set what it has.
        airLevel.target = motorLevel.target = burnerLevel.target = humLevel.target = 0;
        rumbleLevel.target = scrapeLevel.target = grainRate.target = grainMod.target = 0;
        humLevel.target = 0.010f * heat * (gas ? 0.0f : 1.0f);

        switch (kind) {
            case RoasterKind::FluidBed: {
                motorF.target = popper ? 120.0f + 330.0f * fan : 75.0f + 210.0f * fan;
                motorLevel.target = (popper ? 0.055f : 0.028f) * std::sqrt(fan);
                motorBright.target = popper ? 1.0f : 0.4f;
                airLevel.target = (popper ? 0.10f : 0.065f) * fan;
                airFc.target = 500.0f + 2200.0f * fan;
                if (inMachine) {
                    float agit = static_cast<float>(t.agitation);
                    bool stalled = agit < 0.7f;
                    grainRate.target = stalled ? 15.0f : (50.0f + 300.0f * clamp01f(agit / 2.0f)) * std::sqrt(massNorm);
                    grainF.target = 3200.0f * (7.5f / size) * bright * (stalled ? 0.6f : 1.0f);
                    grainAmp.target = 0.05f * std::sqrt(hardness);
                    grainTau = 0.004f;
                }
                crackMuffle = 1.0f;
                crackLp = 1.0f;
                break;
            }
            case RoasterKind::Drum: {
                float rpm = static_cast<float>(t.drum_rpm);
                float rpmNorm = clamp01f(rpm / std::max(1.0f, rpmMax));
                rumbleLevel.target = rpm > 0.5f ? 0.06f + 0.06f * rpmNorm : 0.0f;
                rumbleHz.target = rpm / 60.0f * 4.0f;  // four vanes lift the beans per turn
                motorF.target = 95.0f;
                motorLevel.target = rpm > 0.5f ? 0.025f : 0.0f;
                motorBright.target = 0.3f;
                airLevel.target = 0.02f + 0.06f * fan;
                airFc.target = 350.0f + 900.0f * fan;
                burnerLevel.target = gas ? 0.12f * std::pow(heat, 0.7f) : 0.0f;
                if (inMachine && rpm > 0.5f) {
                    grainRate.target = (30.0f + 200.0f * std::sqrt(massNorm)) * clamp01f(rpmNorm * 1.5f);
                    grainMod.target = 0.7f;
                    grainF.target = 1700.0f * (7.5f / size) * bright;
                    grainAmp.target = 0.10f * std::sqrt(hardness);
                    grainTau = 0.006f;
                }
                crackMuffle = 0.8f;  // inside a steel drum, under the burner
                crackLp = 0.8f;
                break;
            }
            case RoasterKind::StirPan: {
                float stir = static_cast<float>(t.stir_rpm);
                float stirNorm = clamp01f(stir / std::max(1.0f, stirMax));
                motorF.target = 60.0f + 90.0f * stirNorm;
                motorLevel.target = stir > 0.1f ? 0.035f : 0.0f;
                motorBright.target = 0.6f;
                scrapeLevel.target = 0.08f * stirNorm * (inMachine ? 1.0f : 0.5f);
                scrapeHz.target = stir / 60.0f * 2.0f;
                airLevel.target = 0.05f * fan;
                airFc.target = 1200.0f + 1500.0f * fan;
                if (inMachine) {
                    grainRate.target = (15.0f + 160.0f * stirNorm) * std::sqrt(massNorm);
                    grainF.target = 1500.0f * (7.5f / size) * bright;
                    grainAmp.target = 0.09f * std::sqrt(hardness);
                    grainTau = 0.006f;
                }
                crackMuffle = 0.65f;  // under the dome
                crackLp = 0.9f;
                break;
            }
        }
        // Cooling tray: its own fan and stirring arm once the beans drop.
        if (inTray && s.actuators[static_cast<int>(Control::Cooling)] >= 50) {
            airLevel.target += 0.04f;
            airFc.target = std::max(airFc.target, 900.0f);
            grainRate.target = 60.0f * std::sqrt(std::max(0.2f, massNorm));
            grainF.target = 1400.0f * (7.5f / size) * bright;
            grainAmp.target = 0.035f * std::sqrt(hardness);
            grainMod.target = 0.0f;
        }

        // Relay / bimetal clicks: the thermostat opening or closing, the fuse going.
        std::uint32_t changed = (t.alarms ^ lastTeleAlarms) & (AlarmThermostatOpen | AlarmFuseBlown);
        if (changed) spawn(3000.0f, 3.0f, 0.003f, 0.35f, 0, true);
        lastTeleAlarms = t.alarms;

        scheduleCracks(inMachine);
    }

    void scheduleCracks(bool inMachine) {
        auto cracks = host.cracksSince(lastCrack, 600);
        if (cracks.empty()) return;
        lastCrack = cracks.back().seq;
        // At high sim speed or peak first crack in a big batch there can be
        // hundreds per refresh; voice a sample of them, a little louder each.
        const std::size_t maxVoiced = 40;
        float boost = cracks.size() > maxVoiced ? std::min(2.0f, std::sqrt(static_cast<float>(cracks.size()) / maxVoiced)) : 1.0f;
        const int window = static_cast<int>(0.05f * fs);  // sim steps are 50 ms: spread across one
        for (std::size_t i = 0; i < cracks.size(); ++i) {
            if (cracks.size() > maxVoiced && noise.unit() > static_cast<float>(maxVoiced) / cracks.size()) continue;
            const CrackEvent& c = cracks[i];
            bool muffled = c.where == BeanLocation::Machine;
            float muffle = muffled ? crackMuffle : 1.0f, lp = muffled ? crackLp : 1.0f;
            int delay = static_cast<int>(noise.unit() * window);
            float vary = 0.8f + 0.4f * noise.unit();
            float level = c.loudness * boost * muffle * (0.6f + 0.4f * noise.unit());
            if (!c.second) {
                // First crack: a sharp snap with a woody body. Bigger beans pop
                // lower, harder beans ring higher and crisper, wetter beans
                // burst with more steam (longer, fuller, with a low thump).
                float f = 1300.0f * std::pow(7.5f / c.size_mm, 0.8f) * std::sqrt(c.hardness) * vary * lp;
                float q = 2.0f + 2.5f * c.hardness;
                float tau = 0.010f * (c.size_mm / 7.5f) * (0.7f + 5.0f * c.moisture);
                float thump = 0.30f * level * std::min(1.5f, c.moisture / 0.05f);
                spawn(f, q, tau, 0.9f * level, delay, true, 170.0f * (7.5f / c.size_mm), thump, 0.012f);
            } else {
                // Second crack: fast, fine, high crackle like crisped rice.
                float f = 4500.0f * vary * std::pow(c.hardness, 0.3f) * lp;
                spawn(f, 1.6f, 0.0025f, 0.40f * level, delay, true);
                if (noise.unit() < 0.3f)  // they often come in tiny clusters
                    spawn(f * 1.15f, 1.6f, 0.002f, 0.25f * level, delay + static_cast<int>((0.005f + 0.015f * noise.unit()) * fs), true);
            }
        }
        (void)inMachine;
    }

    static float clamp01f(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

    void render(float* out, int frames) {
        AudioMix m;
        {
            std::lock_guard<std::mutex> lk(mixMu);
            m = mix;
        }
        for (int i = 0; i < frames; ++i) {
            if (--untilRefresh <= 0) {
                refresh();
                untilRefresh = static_cast<int>(fs / 40.0f);
                airBp.set(fs, airFc.v > 50 ? airFc.v : 800.0f, 0.7f, 1);
                burnerLp.set(fs, 380.0f, 0.7f, 0);
                rumbleLp.set(fs, 140.0f, 0.7f, 0);
                scrapeBp.set(fs, 900.0f, 1.2f, 1);
            }
            for (Param* p : {&airLevel, &airFc, &motorLevel, &motorF, &motorBright, &burnerLevel, &humLevel, &rumbleLevel,
                             &rumbleHz, &scrapeLevel, &scrapeHz, &grainRate, &grainF, &grainAmp, &grainMod})
                p->tick(smoothK);

            // ---- machine
            float mach = 0;
            if (airLevel.v > 1e-4f) mach += airBp.run(noise.white()) * airLevel.v * 4.0f;
            if (motorLevel.v > 1e-4f) {
                motorPh += kTwoPi * motorF.v / fs;
                if (motorPh > kTwoPi) motorPh -= kTwoPi;
                float b = motorBright.v;
                float w = std::sin(motorPh) + 0.5f * std::sin(2 * motorPh) + 0.3f * b * std::sin(3 * motorPh) +
                          0.25f * b * std::sin(5 * motorPh);
                mach += w * motorLevel.v * (0.95f + 0.05f * noise.white());
            }
            if (burnerLevel.v > 1e-4f) {
                brown = 0.995f * brown + 0.06f * noise.white();
                flutterPh += kTwoPi * 7.0f / fs;
                if (flutterPh > kTwoPi) flutterPh -= kTwoPi;
                float flutter = 1.0f + 0.2f * std::sin(flutterPh) + 0.1f * noise.white();
                mach += burnerLp.run(brown) * burnerLevel.v * flutter * 3.0f;
            }
            if (humLevel.v > 1e-4f) {
                humPh += kTwoPi * 2.0f * mains / fs;
                if (humPh > kTwoPi) humPh -= kTwoPi;
                mach += (std::sin(humPh) + 0.3f * std::sin(2 * humPh)) * humLevel.v;
            }
            float lift = 1.0f;
            if (rumbleLevel.v > 1e-4f || grainMod.v > 1e-3f) {
                rumblePh += kTwoPi * rumbleHz.v / fs;
                if (rumblePh > kTwoPi) rumblePh -= kTwoPi;
                lift = 0.5f + 0.5f * std::sin(rumblePh);
                mach += rumbleLp.run(noise.white()) * rumbleLevel.v * (0.7f + 0.3f * lift) * 5.0f;
            }
            if (scrapeLevel.v > 1e-4f) {
                scrapePh += kTwoPi * scrapeHz.v / fs;
                if (scrapePh > kTwoPi) scrapePh -= kTwoPi;
                float sweep = std::fabs(std::sin(scrapePh));
                mach += scrapeBp.run(noise.white()) * scrapeLevel.v * (0.4f + 0.6f * sweep) * 3.0f;
            }
            // Beans: individual impacts as a Poisson stream; drums cascade in
            // waves as each vane tips its load.
            if (grainRate.v > 0.5f) {
                float rate = grainRate.v * (1.0f - grainMod.v + grainMod.v * 2.0f * lift);
                if (noise.unit() < rate / fs)  // Poisson process, one sample at a time
                    spawn(grainF.v * (0.75f + 0.5f * noise.unit()), 3.0f, grainTau * (0.6f + 0.8f * noise.unit()),
                          grainAmp.v * (0.4f + 0.6f * noise.unit()), 0, false);
            }

            // ---- voices (bean impacts + cracks)
            float grains = 0, cracks = 0;
            for (auto& v : voices) {
                if (!v.on) continue;
                float x = v.tick(noise);
                if (v.crack) cracks += x;
                else grains += x;
            }
            float y = (mach + grains) * m.machine + cracks * m.cracks;
            out[i] = softClip(y * m.master);
        }
    }
};

AudioSynth::AudioSynth(Host& host, int sampleRate) : impl_(std::make_unique<Impl>(host, sampleRate)) {}
AudioSynth::~AudioSynth() = default;

void AudioSynth::render(float* out, int frames) { impl_->render(out, frames); }

void AudioSynth::setMix(const AudioMix& m) {
    std::lock_guard<std::mutex> lk(impl_->mixMu);
    impl_->mix = m;
}

AudioMix AudioSynth::mix() const {
    std::lock_guard<std::mutex> lk(impl_->mixMu);
    return impl_->mix;
}

int AudioSynth::sampleRate() const { return static_cast<int>(impl_->fs); }

}  // namespace roastsim
