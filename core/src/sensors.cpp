#include "roastsim/sensors.hpp"

#include <cctype>
#include <cmath>

namespace roastsim {

const char* probeFaultName(ProbeFault f) {
    switch (f) {
        case ProbeFault::None: return "none";
        case ProbeFault::Open: return "open";
        case ProbeFault::Short: return "short";
        case ProbeFault::Spikes: return "spikes";
        case ProbeFault::Drift: return "drift";
        case ProbeFault::Stuck: return "stuck";
        case ProbeFault::Noisy: return "noisy";
    }
    return "?";
}

bool probeFaultFromName(const std::string& s, ProbeFault& out) {
    std::string n;
    for (char c : s) n.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (n == "none" || n == "ok" || n == "clear") out = ProbeFault::None;
    else if (n == "open") out = ProbeFault::Open;
    else if (n == "short") out = ProbeFault::Short;
    else if (n == "spikes" || n == "spike") out = ProbeFault::Spikes;
    else if (n == "drift") out = ProbeFault::Drift;
    else if (n == "stuck") out = ProbeFault::Stuck;
    else if (n == "noisy" || n == "noise") out = ProbeFault::Noisy;
    else return false;
    return true;
}

void Probe::setFault(ProbeFault f, double param) {
    fault_ = f;
    faultParam_ = param;
    if (f == ProbeFault::Stuck) faultParam_ = reading_;
    if (f == ProbeFault::None) drift_ = 0;
}

void Probe::step(double dt, double T_env, double immersion, Rng& rng) {
    double tau = def_.tau_air_s + (def_.tau_bean_s - def_.tau_air_s) * immersion;
    tau = std::max(0.05, tau);
    T_ += (T_env - T_) * (1.0 - std::exp(-dt / tau));

    double v = T_ + def_.offset;
    double sd = def_.noise_sd;
    switch (fault_) {
        case ProbeFault::Open: reading_ = -1.0; return;
        case ProbeFault::Short: v = faultParam_ != 0 ? faultParam_ : 25.0; break;  // junction reads the cold end
        case ProbeFault::Spikes:
            if (rng.uniform() < dt * 0.5) v += (rng.uniform() < 0.5 ? -1 : 1) * (40 + 80 * rng.uniform());
            break;
        case ProbeFault::Drift:
            drift_ += dt * (faultParam_ != 0 ? faultParam_ : 0.05);  // C per second
            v += drift_;
            break;
        case ProbeFault::Stuck: reading_ = faultParam_; return;
        case ProbeFault::Noisy: sd = std::max(sd, 2.0); break;
        case ProbeFault::None: break;
    }
    v += sd * rng.normal();
    if (def_.resolution > 0) v = std::round(v / def_.resolution) * def_.resolution;
    reading_ = v;
}

}  // namespace roastsim
