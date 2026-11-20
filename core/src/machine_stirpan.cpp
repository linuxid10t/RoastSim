// Stir-arm pan roaster: a heated nonstick pan, a rotating stir arm and a
// vented dome (optionally with a small fan).
//
// Heat reaches beans mostly by conduction from the pan to the bottom layer;
// the stir arm decides how often each bean gets its turn on the pan. With the
// arm stopped the same beans stay down and scorch. The dome air is solved
// quasi-steadily from pan, beans, vent/fan and dome losses.
#include <cmath>

#include "machines.hpp"

namespace roastsim {

StirPanMachine::StirPanMachine(const RoasterDef& def) : MachineModel(def) {}

void StirPanMachine::reset(const Environment& env) {
    ambient_ = env.ambient_C;
    T_pan_ = T_dome_ = env.ambient_C;
    pwmPhase_ = 0;
    thermostatOpen_ = false;
    tel_ = MachineTelemetry{};
    tel_.inlet_C = tel_.exhaust_C = tel_.air_C = tel_.wall_C = tel_.element_C = env.ambient_C;
}

void StirPanMachine::step(double dt, MachineInputs& in) {
    const StirPanParams& P = def_.pan;
    const Environment& env = *in.env;
    const double Ta = env.ambient_C;
    ambient_ = Ta;
    std::uint32_t alarms = 0;

    // ---- heater (resistive element bonded to the pan)
    const double Rh = P.nominal_V * P.nominal_V / std::max(1.0, P.heater_W);
    double V = env.line_voltage * Rh / (Rh + env.source_ohms);
    double full = thermostatOpen_ ? 0.0 : V * V / Rh;
    double duty = in.u[static_cast<int>(Control::Heat)];
    double heaterP;
    if (P.pwm_period_s > 0) {
        pwmPhase_ = std::fmod(pwmPhase_ + dt, P.pwm_period_s);
        heaterP = pwmPhase_ < duty * P.pwm_period_s ? full : 0.0;
    } else {
        heaterP = full * duty;
    }

    // ---- stir arm and dome fan
    double stirU = def_.hasControl(Control::Stir) ? in.u[static_cast<int>(Control::Stir)] : 0.6;
    double rpm = P.stir_rpm_max * stirU;
    angle_ = std::fmod(angle_ + rpm / 60.0 * 2.0 * kPi * dt, 2.0 * kPi);
    double fan = def_.hasControl(Control::Fan) ? in.u[static_cast<int>(Control::Fan)] : 0.0;
    double rho = airDensityRatio(env.altitude_m) * (293.15 / toK(Ta));
    double ventUA = P.dome_vent_UA + P.fan_max_flow * fan * rho * kAirCp;

    const double panArea = kPi * 0.25 * P.pan_diameter_m * P.pan_diameter_m;
    double Qpan_beans = 0;
    double cover = 0;
    BeanBatch* beans = in.beans;
    if (beans && !beans->empty()) {
        const Kinetics& k = beans->kinetics();
        auto& parcels = beans->parcels();
        double N = beans->totalBeans();
        double dMean = 0;
        for (const auto& p : parcels) dMean += p.count * p.d;
        dMean /= std::max(1e-9, N);
        double footprint = 0.8 * dMean * dMean;
        double monolayer = 0.9 * panArea / footprint;
        cover = std::min(1.0, N / monolayer);
        double contactMean = std::min(1.0, monolayer / std::max(1.0, N));

        double q = smoothstep(0.3, 6.0, rpm);
        double tauMix = rpm > 0.05 ? std::max(2.0, 30.0 / rpm) : 400.0;
        beans->updateMixing(dt, tauMix, *in.rng);
        tel_.mix_quality = q;
        tel_.agitation = rpm / std::max(1e-9, P.stir_rpm_max);
        tel_.fill = cover;

        double sDyn = 0.6;
        double sSt = P.unevenness * (1.0 + 4.0 * (1.0 - q));
        double cSum = 0, nSum = 0;
        for (auto& p : parcels) {
            double c = std::max(0.0, 1.0 + sDyn * p.z_contact + sSt * p.bias);
            p.spot_T = c;  // stash
            cSum += p.count * c;
            nSum += p.count;
        }
        double cNorm = nSum / std::max(1e-12, cSum);  // make the mean weight 1
        double sumHA = 0, sumHAT = 0;
        for (auto& p : parcels) {
            double contact = std::min(1.0, contactMean * p.spot_T * cNorm);
            double hA = P.h_dome * p.area(k) * (1.0 - 0.5 * contact);
            sumHA += p.count * hA;
            sumHAT += p.count * hA * p.T_s;
            p.q_in = contact;  // stash
        }
        double panAirUA = P.pan_air_UA * (1.0 - 0.6 * cover);
        T_dome_ = (panAirUA * T_pan_ + sumHAT + (ventUA + P.dome_loss_UA) * Ta) /
                  (panAirUA + sumHA + ventUA + P.dome_loss_UA);
        double envU = 0, envUT = 0;  // for tel_.bean_env_C
        for (auto& p : parcels) {
            double contact = p.q_in;
            double UAc = P.h_contact * 0.8 * p.d * p.d * contact;
            double hA = P.h_dome * p.area(k) * (1.0 - 0.5 * contact);
            double qc = UAc * (T_pan_ - p.T_s);
            p.q_in = qc + hA * (T_dome_ - p.T_s);
            Qpan_beans += p.count * qc;
            p.spot_T = p.T_s + (T_pan_ - p.T_s) * (P.h_contact / (P.h_contact + 200.0)) * std::min(1.0, 2.0 * contact);
            envU += p.count * (UAc + hA);
            envUT += p.count * (UAc * T_pan_ + hA * T_dome_);
        }
        // Equilibrium of pan contact + dome air: the beans' target.
        tel_.bean_env_C = envU > 0 ? envUT / envU : -1;
        if (rpm < 0.3) {
            alarms |= AlarmBedStalled;
            tel_.bed_state = "static";
        } else {
            tel_.bed_state = "stirred";
        }
        tel_.bt_immersion = 0.9 * smoothstep(0.2 * P.bt_probe_full_g, P.bt_probe_full_g, beans->totalMass() * 1000.0);
    } else {
        T_dome_ = (P.pan_air_UA * T_pan_ + (ventUA + P.dome_loss_UA) * Ta) / (P.pan_air_UA + ventUA + P.dome_loss_UA);
        tel_.fill = 0;
        tel_.mix_quality = 1;
        tel_.agitation = rpm / std::max(1e-9, P.stir_rpm_max);
        tel_.bt_immersion = 0;
        tel_.bed_state = "empty";
        tel_.bean_env_C = -1;
    }

    double panAirUA = P.pan_air_UA * (1.0 - 0.6 * cover);
    T_pan_ += (heaterP - Qpan_beans - panAirUA * (T_pan_ - T_dome_) - P.pan_loss_UA * (T_pan_ - Ta)) / P.pan_C * dt;

    if (P.thermostat) {
        if (!thermostatOpen_ && T_pan_ > P.thermostat_open_C) thermostatOpen_ = true;
        else if (thermostatOpen_ && T_pan_ < P.thermostat_close_C) thermostatOpen_ = false;
    }
    if (thermostatOpen_) alarms |= AlarmThermostatOpen;
    if (T_pan_ > 330) alarms |= AlarmOverheat;

    tel_.heater_W = heaterP;
    tel_.voltage_V = V;
    tel_.airflow_kg_s = P.fan_max_flow * fan * rho;
    tel_.inlet_C = Ta;
    tel_.exhaust_C = T_dome_;
    tel_.air_C = T_dome_;
    tel_.wall_C = T_pan_;
    tel_.element_C = T_pan_;
    tel_.stir_rpm = rpm;
    tel_.angle_rad = angle_;
    tel_.alarms = alarms;
}

double StirPanMachine::probeEnv(ProbeLocation loc, const BeanBatch* beans, double& immersion) const {
    immersion = 0;
    switch (loc) {
        case ProbeLocation::BeanMass:
            if (beans && !beans->empty()) {
                immersion = tel_.bt_immersion;
                // A probe near the pan floor also feels the pan a little.
                double Tb = immersion * beans->meanSurfaceT() + (1.0 - immersion) * T_dome_;
                return Tb + 0.05 * (T_pan_ - Tb);
            }
            return T_dome_ + 0.1 * (T_pan_ - T_dome_);
        case ProbeLocation::InletAir:
        case ProbeLocation::ExhaustAir:
        case ProbeLocation::ChamberAir: return T_dome_;
        case ProbeLocation::Wall:
        case ProbeLocation::Element: return T_pan_;
        case ProbeLocation::Ambient: return ambient_;
    }
    return T_dome_;
}

// ---------------------------------------------------------------- factory
std::unique_ptr<MachineModel> createMachine(const RoasterDef& def) {
    switch (def.kind) {
        case RoasterKind::FluidBed: return std::make_unique<FluidBedMachine>(def);
        case RoasterKind::Drum: return std::make_unique<DrumMachine>(def);
        case RoasterKind::StirPan: return std::make_unique<StirPanMachine>(def);
    }
    return nullptr;
}

}  // namespace roastsim
