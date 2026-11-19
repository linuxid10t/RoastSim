// Shared enums, constants and small helpers for the roast simulation core.
// The core is pure C++17: no OS headers, no threads, no I/O beyond what the
// caller hands in. That keeps it portable to Windows, Linux and Haiku.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace roastsim {

// ---------------------------------------------------------------- constants
constexpr double kAirCp = 1006.0;           // J/(kg K)
constexpr double kWaterCp = 4186.0;         // J/(kg K)
constexpr double kSigma = 5.670374419e-8;   // Stefan-Boltzmann W/(m^2 K^4)
constexpr double kKelvin = 273.15;
constexpr double kPi = 3.14159265358979323846;

inline double toK(double c) { return c + kKelvin; }
inline double clamp01(double v) { return std::min(1.0, std::max(0.0, v)); }
inline double clampd(double v, double lo, double hi) { return std::min(hi, std::max(lo, v)); }
inline double lerp(double a, double b, double t) { return a + (b - a) * t; }
inline double smoothstep(double e0, double e1, double x) {
    double t = clamp01((x - e0) / (e1 - e0));
    return t * t * (3.0 - 2.0 * t);
}
// Air density ratio relative to sea level (isothermal barometric approximation).
inline double airDensityRatio(double altitude_m) { return std::exp(-altitude_m / 8434.0); }

// ---------------------------------------------------------------- controls
// Every roaster exposes a subset of these actuators, each driven 0..100 %.
enum class Control : int {
    Heat = 0,   // heater power / gas valve / burner
    Fan,        // airflow, exhaust fan, damper
    Drum,       // drum speed
    Stir,       // stir-arm speed
    Voltage,    // variac / line voltage applied to a stock appliance
    Cooling,    // cooling tray fan + stirrer (on/off, 0 or 100)
    Count
};
constexpr int kControlCount = static_cast<int>(Control::Count);

const char* controlName(Control c);
bool controlFromName(const std::string& name, Control& out);  // accepts aliases (burner, gas, air...)

// ---------------------------------------------------------------- kinds
enum class RoasterKind { FluidBed, Drum, StirPan };
const char* roasterKindName(RoasterKind k);
bool roasterKindFromName(const std::string& s, RoasterKind& out);

enum class ProbeLocation {
    BeanMass,    // thermocouple sitting in the bean pile (classic BT)
    InletAir,    // hot air entering the bean chamber
    ExhaustAir,  // air leaving the chamber / drum
    ChamberAir,  // air inside the drum / dome
    Wall,        // drum shell or pan surface
    Element,     // heater element / burner area
    Ambient
};
const char* probeLocationName(ProbeLocation l);
bool probeLocationFromName(const std::string& s, ProbeLocation& out);

enum class RoastPhase { Idle = 0, Roasting = 1, Cooling = 2, Done = 3 };
const char* roastPhaseName(RoastPhase p);

// Where the beans currently are.
enum class BeanLocation { None = 0, Machine = 1, Tray = 2 };

// ---------------------------------------------------------------- environment
struct Environment {
    double ambient_C = 22.0;
    double humidity_rh = 45.0;      // %
    double line_voltage = 120.0;    // V RMS at the wall (no load)
    double source_ohms = 0.25;      // wiring/source impedance -> voltage sag under load
    double altitude_m = 0.0;
    double gas_pressure = 1.0;      // relative to nominal supply pressure
};

// ---------------------------------------------------------------- alarms
enum Alarm : std::uint32_t {
    AlarmNone = 0,
    AlarmThermostatOpen = 1u << 0,   // bimetal cut-out currently open
    AlarmFuseBlown = 1u << 1,        // thermal fuse blown: heater dead until repaired
    AlarmBeansEjecting = 1u << 2,    // airflow so high beans leave the chamber
    AlarmBedStalled = 1u << 3,       // airflow too low to fluidize / no agitation
    AlarmScorching = 1u << 4,        // hot contact surfaces burning beans
    AlarmSmokeHeavy = 1u << 5,
    AlarmFireRisk = 1u << 6,         // beans/chaff hot enough to ignite
    AlarmOverheat = 1u << 7,         // machine structure beyond its rating
    AlarmCentrifuging = 1u << 8,     // drum too fast, beans pinned to wall
    AlarmProbeFault = 1u << 9,
};

}  // namespace roastsim
