// Fluid-bed air roaster and popcorn popper.
//
// Heater element (thermal node) -> inlet air -> bean bed (NTU exchange with
// bypass captured by an effective h) -> exhaust, with a chamber wall node.
// "stock" wiring runs heater and fan motor from one (variac) supply, with an
// optional bimetal thermostat and thermal fuse. "independent" wiring is the
// modded/air-roaster case with separate heater duty (optionally SSR PWM) and fan.
#include <cmath>

#include "machines.hpp"

namespace roastsim {

namespace {

struct BedCirculation {
    double lift;          // how much of the airflow lifts the bed
    double turnover_s;    // exposure correlation time once the bed is fully moving
    double concentration; // relative difference in exposure across the bed
    double carryover;     // tendency to carry beans out at high flow
};

BedCirculation circulationFor(FluidBedAirPattern pattern) {
    switch (pattern) {
        case FluidBedAirPattern::CentralSpout:     return {1.12, 2.8, 1.65, 1.18};
        case FluidBedAirPattern::TangentialSwirl: return {0.88, 1.2, 0.70, 0.82};
        case FluidBedAirPattern::SideJet:          return {1.03, 1.8, 1.35, 1.04};
        case FluidBedAirPattern::Upflow:          return {1.00, 2.0, 1.00, 1.00};
    }
    return {1.00, 2.0, 1.00, 1.00};
}

}  // namespace

FluidBedMachine::FluidBedMachine(const RoasterDef& def) : MachineModel(def) {}

void FluidBedMachine::reset(const Environment& env) {
    const double Ta = env.ambient_C;
    ambient_ = Ta;
    T_el_ = T_wall_ = T_in_ = T_out_ = T_airEff_ = Ta;
    flow_ = 0;
    pwmPhase_ = 0;
    thermostatOpen_ = false;
    fuseTimer_ = 0;
    tel_ = MachineTelemetry{};
    tel_.inlet_C = tel_.exhaust_C = tel_.air_C = tel_.wall_C = tel_.element_C = Ta;
}

void FluidBedMachine::repair() {
    fuseBlown_ = false;
    fuseTimer_ = 0;
}

void FluidBedMachine::step(double dt, MachineInputs& in) {
    const FluidBedParams& P = def_.fluid;
    const Environment& env = *in.env;
    const double Ta = env.ambient_C;
    ambient_ = Ta;
    const bool stock = (P.wiring == "stock");
    std::uint32_t alarms = 0;

    // ---- electrical supply
    const double Rh = P.nominal_V * P.nominal_V / std::max(1.0, P.heater_W);
    double Vsupply = env.line_voltage;
    double heaterP = 0, fanFrac = 0;
    bool heaterClosed = !thermostatOpen_ && !fuseBlown_;
    if (stock) {
        double variac = in.u[static_cast<int>(Control::Voltage)];
        double Rload = Rh / 1.12;  // motor draws ~12 % extra current
        double Vout = Vsupply * variac;
        double V = Vout * Rload / (Rload + env.source_ohms);
        tel_.voltage_V = V;
        if (heaterClosed) heaterP = V * V / Rh;
        double vr = V / std::max(1.0, P.nominal_V);
        fanFrac = clamp01((vr - P.stock_stall_frac) / (1.0 - P.stock_stall_frac));
    } else {
        double duty = in.u[static_cast<int>(Control::Heat)];
        double V = Vsupply * Rh / (Rh + env.source_ohms);
        tel_.voltage_V = V;
        double full = heaterClosed ? V * V / Rh : 0.0;
        if (P.pwm_period_s > 0) {
            pwmPhase_ += dt;
            if (pwmPhase_ >= P.pwm_period_s) pwmPhase_ = std::fmod(pwmPhase_, P.pwm_period_s);
            heaterP = (pwmPhase_ < duty * P.pwm_period_s) ? full : 0.0;
        } else {
            heaterP = full * duty;
        }
        double fan = in.u[static_cast<int>(Control::Fan)];
        const auto* fanControl = def_.control(Control::Fan);
        if (fan > 0 && fanControl && fanControl->levels > 0) {
            // Numbered settings span the fan's running range above its stall threshold.
            const double minimum = clamp01(P.fan_min_pct / 100.0);
            fanFrac = minimum + (1.0 - minimum) * fan;
        } else {
            fanFrac = (fan * 100.0 < P.fan_min_pct) ? 0.0 : fan;
        }
    }

    // ---- air flow (mass flow drops with altitude and with hot intake air)
    double rho = airDensityRatio(env.altitude_m) * (293.15 / toK(Ta));
    double flowTarget = P.fan_max_flow * fanFrac * rho;
    flow_ += (flowTarget - flow_) * (1.0 - std::exp(-dt / std::max(0.05, P.fan_tau)));
    const double airCapacityRate = flow_ * kAirCp;
    const double mcp = std::max(1e-6, airCapacityRate);

    // ---- heater element
    T_in_ = Ta + P.element_eff * (T_el_ - Ta);
    double QelAir = airCapacityRate * (T_in_ - Ta);
    double QelLoss = P.element_loss_UA * (T_el_ - Ta);
    T_el_ += (heaterP - QelAir - QelLoss) / P.element_C * dt;

    // ---- bean bed
    BeanBatch* beans = in.beans;
    if (beans && !beans->empty()) {
        const Kinetics& k = beans->kinetics();
        auto& parcels = beans->parcels();
        double M = beans->totalMass();
        // Roasted beans are lighter and bigger -> easier to lift.
        double ratioSum = 0, nSum = 0;
        for (const auto& p : parcels) {
            double e = p.d / p.d0;
            double densRatio = (p.beanMass() / (p.m_dry0 * (1 + p.X0) + p.chaff0)) / (e * e * e);
            ratioSum += p.count * std::sqrt(densRatio * e);
            nSum += p.count;
        }
        double liftFactor = nSum > 0 ? ratioSum / nSum : 1.0;
        double required = std::max(1e-6, P.fluidize_flow_per_kg * M * liftFactor * rho);
        const auto pattern = circulationFor(P.air_pattern);
        double a = flow_ / required * pattern.lift;
        double q = smoothstep(0.55, 1.05, a);
        tel_.agitation = a;
        tel_.mix_quality = q;
        beans->updateMixing(dt, lerp(45.0, pattern.turnover_s, q), *in.rng);

        double h = P.h_ref * std::pow(std::max(1e-6, flow_ / P.fan_max_flow), P.h_flow_exp) * (0.55 + 0.45 * q);
        double sDyn = (0.15 + 0.5 * (1.0 - q)) * pattern.concentration;
        double sSt = P.unevenness * (1.0 + 2.0 * (1.0 - q)) * pattern.concentration;

        double sumHA = 0, sumHAT = 0;
        for (auto& p : parcels) {
            double hA = h * p.area(k) * exposure(p, sDyn, sSt);
            p.q_in = hA;  // stash conductance; converted to watts below
            sumHA += p.count * hA;
            sumHAT += p.count * hA * p.T_s;
        }
        double Tbw = sumHA > 0 ? sumHAT / sumHA : T_in_;
        double NTU = sumHA / mcp;
        double eps = 1.0 - std::exp(-NTU);
        T_airEff_ = NTU > 1e-9 ? Tbw + (T_in_ - Tbw) * eps / NTU : T_in_;
        T_out_ = T_in_ - eps * (T_in_ - Tbw);
        for (auto& p : parcels) {
            double hA = p.q_in;
            double e = hA / std::max(1e-12, h * p.area(k));
            p.q_in = hA * (T_airEff_ - p.T_s);
            // Hot spot: a moving bed keeps the beans off the inlet screen. As
            // it stalls, the bottom (most exposed) beans rest in the jet of
            // inlet air straight off the heater, far hotter than the bed.
            const double resting = (1.0 - q) * std::min(1.0, 0.5 * e);
            p.spot_T = p.T_s + resting * std::max(0.0, T_in_ - p.T_s);
        }
        // Beans only cool the air while they are colder than it: they head
        // for the inlet air temperature.
        tel_.bean_env_C = T_in_;

        const double carryover = a * pattern.carryover;
        if (a < 0.7) {
            alarms |= AlarmBedStalled;
            tel_.bed_state = "stalled";
        } else if (carryover > P.eject_agitation) {
            alarms |= AlarmBeansEjecting;
            tel_.bed_state = "ejecting";
            beans->removeFraction(0.004 * (carryover - P.eject_agitation) * dt);
        } else {
            tel_.bed_state = a < 1.0 ? "sluggish" : "fluidized";
        }
        double chamberVol = kPi * 0.25 * P.chamber_diameter_m * P.chamber_diameter_m * P.chamber_height_m;
        tel_.fill = beans->bulkVolume() / std::max(1e-9, chamberVol);
        tel_.bt_immersion = 0.62 + 0.28 * q;
        if (P.bt_probe_full_kg > 0)
            tel_.bt_immersion *= clamp01(beans->greenMass() / P.bt_probe_full_kg);
    } else {
        T_airEff_ = T_out_ = T_in_;
        tel_.agitation = 0;
        tel_.mix_quality = 1;
        tel_.fill = 0;
        tel_.bt_immersion = 0;
        tel_.bed_state = "empty";
        tel_.bean_env_C = -1;
    }

    // ---- chamber wall: exchange with the exhaust stream downstream of the bed.
    // The same heat leaves the air and enters the wall, including during fan coastdown.
    double Qw = 0;
    if (airCapacityRate > 0) {
        double effectiveness = -std::expm1(-std::max(0.0, P.wall_UA_in) / airCapacityRate);
        double dTw = effectiveness * (T_out_ - T_wall_);
        Qw = airCapacityRate * dTw;
        T_out_ -= dTw;
    }
    T_wall_ += (Qw - P.wall_UA_out * (T_wall_ - Ta)) / P.wall_C * dt;

    // ---- protection devices
    if (P.thermostat) {
        if (!thermostatOpen_ && T_in_ > P.thermostat_open_C) thermostatOpen_ = true;
        else if (thermostatOpen_ && T_in_ < P.thermostat_close_C) thermostatOpen_ = false;
    }
    if (P.fuse_C > 0 && !fuseBlown_) {
        if (T_in_ > P.fuse_C) {
            fuseTimer_ += dt;
            if (fuseTimer_ > P.fuse_hold_s) fuseBlown_ = true;
        } else {
            fuseTimer_ = std::max(0.0, fuseTimer_ - dt);
        }
    }
    if (thermostatOpen_) alarms |= AlarmThermostatOpen;
    if (fuseBlown_) alarms |= AlarmFuseBlown;
    if (T_el_ > 900) alarms |= AlarmOverheat;

    tel_.heater_W = heaterP;
    tel_.airflow_kg_s = flow_;
    tel_.inlet_C = T_in_;
    tel_.exhaust_C = T_out_;
    tel_.air_C = T_airEff_;
    tel_.wall_C = T_wall_;
    tel_.element_C = T_el_;
    tel_.alarms = alarms;
}

double FluidBedMachine::probeEnv(ProbeLocation loc, const BeanBatch* beans, double& immersion) const {
    immersion = 0;
    switch (loc) {
        case ProbeLocation::BeanMass:
            if (beans && !beans->empty()) {
                immersion = tel_.bt_immersion;
                return immersion * beans->meanSurfaceT() + (1.0 - immersion) * T_airEff_;
            }
            return T_out_;
        case ProbeLocation::InletAir: return T_in_;
        case ProbeLocation::ExhaustAir: return T_out_;
        case ProbeLocation::ChamberAir: return T_airEff_;
        case ProbeLocation::Wall: return T_wall_;
        case ProbeLocation::Element: return T_el_;
        case ProbeLocation::Ambient: return ambient_;
    }
    return T_out_;
}

}  // namespace roastsim
