#include "roastsim/types.hpp"

#include <cctype>

namespace roastsim {

namespace {
std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
}  // namespace

const char* controlName(Control c) {
    switch (c) {
        case Control::Heat: return "heat";
        case Control::Fan: return "fan";
        case Control::Drum: return "drum";
        case Control::Stir: return "stir";
        case Control::Voltage: return "voltage";
        case Control::Cooling: return "cooling";
        default: return "?";
    }
}

bool controlFromName(const std::string& name, Control& out) {
    std::string n = lower(name);
    if (n == "heat" || n == "heater" || n == "burner" || n == "gas" || n == "power" || n == "ot1") {
        out = Control::Heat;
    } else if (n == "fan" || n == "air" || n == "airflow" || n == "damper" || n == "ot2" || n == "io3") {
        out = Control::Fan;
    } else if (n == "drum" || n == "drum_speed" || n == "rpm") {
        out = Control::Drum;
    } else if (n == "stir" || n == "stirrer" || n == "agitator") {
        out = Control::Stir;
    } else if (n == "voltage" || n == "variac" || n == "line") {
        out = Control::Voltage;
    } else if (n == "cooling" || n == "cool" || n == "cooler") {
        out = Control::Cooling;
    } else {
        return false;
    }
    return true;
}

const char* roasterKindName(RoasterKind k) {
    switch (k) {
        case RoasterKind::FluidBed: return "fluid_bed";
        case RoasterKind::Drum: return "drum";
        case RoasterKind::StirPan: return "stir_pan";
    }
    return "?";
}

bool roasterKindFromName(const std::string& s, RoasterKind& out) {
    std::string n = lower(s);
    if (n == "fluid_bed" || n == "air" || n == "popper") out = RoasterKind::FluidBed;
    else if (n == "drum") out = RoasterKind::Drum;
    else if (n == "stir_pan" || n == "stir" || n == "pan") out = RoasterKind::StirPan;
    else return false;
    return true;
}

const char* probeLocationName(ProbeLocation l) {
    switch (l) {
        case ProbeLocation::BeanMass: return "bean_mass";
        case ProbeLocation::InletAir: return "inlet_air";
        case ProbeLocation::ExhaustAir: return "exhaust_air";
        case ProbeLocation::ChamberAir: return "chamber_air";
        case ProbeLocation::Wall: return "wall";
        case ProbeLocation::Element: return "element";
        case ProbeLocation::Ambient: return "ambient";
    }
    return "?";
}

bool probeLocationFromName(const std::string& s, ProbeLocation& out) {
    std::string n = lower(s);
    if (n == "bean_mass" || n == "beans") out = ProbeLocation::BeanMass;
    else if (n == "inlet_air" || n == "inlet") out = ProbeLocation::InletAir;
    else if (n == "exhaust_air" || n == "exhaust") out = ProbeLocation::ExhaustAir;
    else if (n == "chamber_air" || n == "drum_air" || n == "dome_air") out = ProbeLocation::ChamberAir;
    else if (n == "wall" || n == "drum_wall" || n == "pan") out = ProbeLocation::Wall;
    else if (n == "element" || n == "burner") out = ProbeLocation::Element;
    else if (n == "ambient") out = ProbeLocation::Ambient;
    else return false;
    return true;
}

const char* roastPhaseName(RoastPhase p) {
    switch (p) {
        case RoastPhase::Idle: return "idle";
        case RoastPhase::Roasting: return "roasting";
        case RoastPhase::Cooling: return "cooling";
        case RoastPhase::Done: return "done";
    }
    return "?";
}

}  // namespace roastsim
