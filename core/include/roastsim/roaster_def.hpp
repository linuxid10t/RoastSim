// Data-driven roaster definitions (loaded from data/roasters/*.json).
// A definition picks a machine family (fluid bed, drum, stir pan), the controls
// it exposes, where its probes sit, and the physical parameters of the family.
#pragma once

#include <string>
#include <vector>

#include "roastsim/json.hpp"
#include "roastsim/types.hpp"

namespace roastsim {

struct ControlDef {
    Control id = Control::Heat;
    std::string label;
    std::string unit = "%";
    double def = 0;
    double min = 0, max = 100, step = 1;
    bool toggle = false;      // on/off switch rather than a slider
    double slew_per_s = 0;    // 0 = instant, else max change in % per second (e.g. gas valves)
};

struct ProbeDef {
    std::string id;           // "BT", "ET", "Inlet", ...
    std::string label;
    ProbeLocation loc = ProbeLocation::BeanMass;
    double tau_air_s = 8.0;   // time constant in moving air
    double tau_bean_s = 2.5;  // time constant fully immersed in beans
    double noise_sd = 0.12;
    double resolution = 0.25; // MAX31855-style quantisation
    double offset = 0.0;
};

// Air roaster and popcorn popper.
struct FluidBedParams {
    std::string wiring = "independent";  // "independent" (modded/air roaster) or "stock"
    double heater_W = 1400;
    double nominal_V = 120;
    double element_C = 45;          // J/K element thermal mass
    double element_eff = 0.9;       // heat-exchanger effectiveness element->air
    double element_loss_UA = 0.4;   // W/K element losses to housing
    double fan_max_flow = 0.0062;   // kg/s at 100 % fan, sea level
    double fan_tau = 0.8;
    double fan_min_pct = 15;        // motor stalls below this (independent wiring)
    double stock_stall_frac = 0.45; // stock: motor stalls below this fraction of nominal V
    double wall_C = 350;
    double wall_UA_in = 1.5;
    double wall_UA_out = 1.2;
    double h_ref = 16;              // W/(m^2 K) effective bean heat transfer at max flow
    double h_flow_exp = 0.6;
    double fluidize_flow_per_kg = 0.045;  // kg/s of air needed per kg of green beans
    double eject_agitation = 2.6;
    double unevenness = 0.08;
    bool thermostat = false;        // bimetal cut-out sensing inlet air
    double thermostat_open_C = 240;
    double thermostat_close_C = 215;
    double fuse_C = 0;              // thermal fuse on inlet air (0 = none)
    double fuse_hold_s = 3;
    double pwm_period_s = 0;        // SSR time-proportioning period (0 = smooth)
    double chamber_diameter_m = 0.08;
    double chamber_height_m = 0.16;
};

// Drum roaster (home electric to shop gas).
struct DrumParams {
    std::string heat_source = "gas";  // gas | electric | induction | ir
    double heat_max_W = 15000;
    double heat_min_frac = 0.0;       // minimum flame when on
    double heat_tau = 1.5;
    double efficiency = 0.65;         // fraction of fuel/element energy entering the roaster
    double frac_drum = 0.45;          // share heating the drum shell
    double frac_air = 0.55;           // share heating the air drawn through the drum
    double frac_beans_rad = 0.0;      // share radiated straight at the beans (IR)
    double nominal_V = 0;             // electric: >0 makes power follow line voltage
    double drum_mass_kg = 60;
    double drum_cp = 500;
    double drum_diameter_m = 0.5;
    double drum_length_m = 0.6;
    bool double_wall = true;
    double outer_frac = 0.5;          // mass share of the outer shell (double wall)
    double wall_G = 150;              // W/K outer->inner
    double loss_UA = 8;               // W/K to ambient
    double airflow_max = 0.05;        // kg/s
    double airflow_min_frac = 0.25;
    double fan_tau = 2.0;
    double h_air_ref = 12;            // W/(m^2 K) air->bean at max flow
    double h_flow_exp = 0.6;
    double wall_air_UA = 20;          // W/K drum wall <-> drum air
    double h_contact = 160;           // W/(m^2 K) bed <-> wall contact
    double emissivity = 0.7;
    double rpm_max = 60;
    double unevenness = 0.03;
    double bt_probe_full_kg = 3.0;    // batch at which the BT probe is fully buried
};

// Stir-arm pan roaster.
struct StirPanParams {
    double heater_W = 1200;
    double nominal_V = 120;
    double pan_C = 650;               // J/K pan + element
    double pan_loss_UA = 1.4;
    double pan_diameter_m = 0.24;
    bool thermostat = true;
    double thermostat_open_C = 270;
    double thermostat_close_C = 250;
    double h_contact = 170;
    double h_dome = 10;
    double pan_air_UA = 3.0;
    double dome_vent_UA = 1.2;
    double dome_loss_UA = 1.0;
    double fan_max_flow = 0.004;
    double stir_rpm_max = 12;
    double unevenness = 0.06;
    double pwm_period_s = 0;
    double bt_probe_full_g = 150;
    double dome_height_m = 0.12;
};

struct CoolingParams {
    double h_on = 45;   // W/(m^2 K) forced air in the cooling tray
    double h_off = 10;  // natural convection
};

struct BatchLimits {
    double default_g = 100, min_g = 50, max_g = 150;
};

struct RoasterDef {
    std::string id;
    std::string name;
    std::string description;
    RoasterKind kind = RoasterKind::FluidBed;
    BatchLimits batch;
    std::vector<ControlDef> controls;
    std::vector<ProbeDef> probes;
    FluidBedParams fluid;
    DrumParams drum;
    StirPanParams pan;
    CoolingParams cool;
    double charge_bean_temp_C = -1000;  // < -273: use ambient
    json::Value raw;                    // the JSON it was built from

    const ControlDef* control(Control c) const;
    bool hasControl(Control c) const { return control(c) != nullptr; }
    int probeIndex(const std::string& id) const;

    static bool fromJson(const json::Value& v, RoasterDef& out, std::string* err);
    json::Value describe() const;  // compact description for UIs/clients
};

}  // namespace roastsim
