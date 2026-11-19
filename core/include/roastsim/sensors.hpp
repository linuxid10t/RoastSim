// Thermocouple / RTD probe model: first-order lag whose time constant depends
// on whether the probe is buried in beans or sitting in moving air, plus
// noise, quantisation, calibration offset and injectable faults.
#pragma once

#include <string>

#include "roastsim/roaster_def.hpp"
#include "roastsim/rng.hpp"

namespace roastsim {

enum class ProbeFault { None, Open, Short, Spikes, Drift, Stuck, Noisy };
const char* probeFaultName(ProbeFault f);
bool probeFaultFromName(const std::string& s, ProbeFault& out);

class Probe {
public:
    explicit Probe(const ProbeDef& def) : def_(def) {}

    void reset(double T) { T_ = T; drift_ = 0; }
    void step(double dt, double T_env, double immersion, Rng& rng);
    // Reading as a logger would see it. Open circuit returns -1 (Artisan's "no value").
    double read() const { return reading_; }
    double trueTemp() const { return T_; }

    void setFault(ProbeFault f, double param = 0);
    ProbeFault fault() const { return fault_; }
    const ProbeDef& def() const { return def_; }

private:
    ProbeDef def_;
    double T_ = 20;
    double reading_ = 20;
    ProbeFault fault_ = ProbeFault::None;
    double faultParam_ = 0;
    double drift_ = 0;
    double ambient_ = 20;
};

}  // namespace roastsim
