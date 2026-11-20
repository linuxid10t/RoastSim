// Drum roaster: covers home electric/induction drums and shop gas drums.
//
// Nodes: heat source (first-order lag), drum shell (one or two walls), drum
// air (quasi-steady, solved algebraically each step), beans.
// Heat paths into beans: conduction from the drum wall where the bed touches
// it, convection from the air drawn through the drum, radiation from the hot
// drum interior onto the top of the bed, and (IR heaters) direct radiation.
#include <cmath>

#include "machines.hpp"

namespace roastsim {

DrumMachine::DrumMachine(const RoasterDef& def) : MachineModel(def) {}

void DrumMachine::reset(const Environment& env) {
    ambient_ = env.ambient_C;
    P_src_ = 0;
    T_in_ = T_air_ = T_drum_ = T_outer_ = env.ambient_C;
    flow_ = 0;
    tel_ = MachineTelemetry{};
    tel_.inlet_C = tel_.exhaust_C = tel_.air_C = tel_.wall_C = tel_.element_C = env.ambient_C;
}

void DrumMachine::step(double dt, MachineInputs& in) {
    const DrumParams& P = def_.drum;
    const Environment& env = *in.env;
    const double Ta = env.ambient_C;
    ambient_ = Ta;
    std::uint32_t alarms = 0;
    const double rho = airDensityRatio(env.altitude_m) * (293.15 / toK(Ta));

    // ---- heat source
    double u = in.u[static_cast<int>(Control::Heat)];
    double target = 0;
    if (P.heat_source == "gas") {
        if (u > 0.001) target = P.heat_max_W * (P.heat_min_frac + (1.0 - P.heat_min_frac) * u);
        target *= std::sqrt(std::max(0.0, env.gas_pressure));
        tel_.voltage_V = 0;
    } else {
        double vr = 1.0;
        if (P.nominal_V > 0) {
            double V = env.line_voltage;
            double R = P.nominal_V * P.nominal_V / std::max(1.0, P.heat_max_W);
            V = V * R / (R + env.source_ohms);
            tel_.voltage_V = V;
            vr = V / P.nominal_V;
        }
        // Induction controllers regulate power fairly well; resistive elements follow V^2.
        target = P.heat_max_W * u * (P.heat_source == "induction" ? std::min(1.0, vr) : vr * vr);
    }
    P_src_ += (target - P_src_) * (1.0 - std::exp(-dt / std::max(0.05, P.heat_tau)));
    double Puse = P_src_ * P.efficiency;
    double Pdrum = Puse * P.frac_drum;
    double Pair = Puse * P.frac_air;
    double Pir = Puse * P.frac_beans_rad;

    // ---- airflow
    double fan = in.u[static_cast<int>(Control::Fan)];
    if (!def_.hasControl(Control::Fan)) fan = 0.5;
    double flowTarget = P.airflow_max * (P.airflow_min_frac + (1.0 - P.airflow_min_frac) * fan) * rho;
    flow_ += (flowTarget - flow_) * (1.0 - std::exp(-dt / std::max(0.05, P.fan_tau)));
    const double mcp = std::max(1e-4, flow_ * kAirCp);
    T_in_ = Ta + Pair / mcp;
    if (T_in_ > 1100) T_in_ = 1100;

    // ---- drum rotation
    double rpm = P.rpm_max * (def_.hasControl(Control::Drum) ? in.u[static_cast<int>(Control::Drum)] : 0.75);
    double rpmCrit = 42.3 / std::sqrt(std::max(0.05, P.drum_diameter_m));
    double s = rpm / rpmCrit;
    angle_ = std::fmod(angle_ + rpm / 60.0 * 2.0 * kPi * dt, 2.0 * kPi);

    const double Ainner = kPi * P.drum_diameter_m * P.drum_length_m;
    const double Vdrum = kPi * 0.25 * P.drum_diameter_m * P.drum_diameter_m * P.drum_length_m;
    const double Tdk = toK(T_drum_);

    double Qbeans_from_drum = 0;
    BeanBatch* beans = in.beans;
    if (beans && !beans->empty()) {
        const Kinetics& k = beans->kinetics();
        auto& parcels = beans->parcels();
        double fill = beans->bulkVolume() / Vdrum;
        double q = smoothstep(0.03, 0.25, s) * (1.0 - smoothstep(0.95, 1.15, s));
        tel_.mix_quality = q;
        tel_.agitation = s;
        tel_.fill = fill;
        beans->updateMixing(dt, lerp(120.0, 3.0, q), *in.rng);

        double wet = clampd(0.12 + 0.9 * fill, 0.0, 0.6);
        double UAcontact = P.h_contact * wet * Ainner * (0.7 + 0.3 * q);
        double hAir = P.h_air_ref * std::pow(std::max(1e-6, flow_ / P.airflow_max), P.h_flow_exp) *
                      (0.7 + 0.3 * std::min(1.0, s / 0.5));
        double Atop = 0.7 * P.drum_diameter_m * P.drum_length_m * std::min(1.0, 0.3 + 4.0 * fill);
        double sDyn = 0.25 + 0.8 * (1.0 - q);
        double sSt = P.unevenness * (1.0 + 3.0 * (1.0 - q));

        // Contact and exposure weights
        double cSum = 0, eSum = 0, sumHA = 0, sumHAT = 0, Tbs4 = 0, nSum = 0;
        for (auto& p : parcels) {
            double c = std::max(0.02, 1.0 + sDyn * p.z_contact + sSt * p.bias);
            double e = exposure(p, 0.5 * sDyn, sSt);
            double A = p.area(k);
            cSum += p.count * c;
            eSum += p.count * e * A;
            double hA = hAir * A * e * 0.6;
            sumHA += p.count * hA;
            sumHAT += p.count * hA * p.T_s;
            double Tk = toK(p.T_s);
            Tbs4 += p.count * Tk * Tk * Tk * Tk;
            nSum += p.count;
            p.spot_T = c;  // stash
            p.q_in = e;    // stash
        }
        Tbs4 /= std::max(1e-9, nSum);
        double Qrad = P.emissivity * kSigma * Atop * (Tdk * Tdk * Tdk * Tdk - Tbs4);

        // Quasi-steady drum air
        T_air_ = (mcp * T_in_ + P.wall_air_UA * T_drum_ + sumHAT) / (mcp + P.wall_air_UA + sumHA);

        double envU = 0, envUT = 0;  // for tel_.bean_env_C
        for (auto& p : parcels) {
            double c = p.spot_T, e = p.q_in;
            double A = p.area(k);
            double UAi = UAcontact * c / std::max(1e-12, cSum);
            double share = e * A / std::max(1e-12, eSum);
            double hA = hAir * A * e * 0.6;
            double qc = UAi * (T_drum_ - p.T_s);
            double qr = Qrad * share;
            p.q_in = qc + hA * (T_air_ - p.T_s) + qr + Pir * share;
            envU += p.count * (UAi + hA);
            envUT += p.count * (UAi * T_drum_ + hA * T_air_ + qr + Pir * share);
            Qbeans_from_drum += p.count * (qc + qr);
            // Hot spot where a bean rests on the drum wall.
            double contactFrac = clamp01(c / 2.0);
            p.spot_T = p.T_s + (T_drum_ - p.T_s) * (P.h_contact / (P.h_contact + 200.0)) * contactFrac;
        }
        // Equilibrium of q = sum U (T_src - T_s) + radiation/IR: the beans' target.
        tel_.bean_env_C = envU > 0 ? envUT / envU : -1;
        if (q < 0.25) {
            alarms |= AlarmBedStalled;
            tel_.bed_state = "static";
        } else if (s > 1.0) {
            alarms |= AlarmCentrifuging;
            tel_.bed_state = "centrifuging";
        } else {
            tel_.bed_state = "tumbling";
        }
        tel_.bt_immersion = 0.95 * smoothstep(0.15 * P.bt_probe_full_kg, P.bt_probe_full_kg, beans->totalMass());
    } else {
        T_air_ = (mcp * T_in_ + P.wall_air_UA * T_drum_) / (mcp + P.wall_air_UA);
        tel_.fill = 0;
        tel_.mix_quality = 1;
        tel_.agitation = s;
        tel_.bt_immersion = 0;
        tel_.bed_state = "empty";
        tel_.bean_env_C = -1;
    }

    // ---- drum shell(s)
    double C = P.drum_mass_kg * P.drum_cp;
    double Qair = P.wall_air_UA * (T_drum_ - T_air_);
    if (P.double_wall) {
        double Co = C * P.outer_frac, Ci = C * (1.0 - P.outer_frac);
        double G = P.wall_G * (T_outer_ - T_drum_);
        T_outer_ += (Pdrum - G - P.loss_UA * (T_outer_ - Ta)) / Co * dt;
        T_drum_ += (G - Qair - Qbeans_from_drum) / Ci * dt;
    } else {
        T_drum_ += (Pdrum - Qair - Qbeans_from_drum - P.loss_UA * (T_drum_ - Ta)) / C * dt;
        T_outer_ = T_drum_;
    }
    if (T_drum_ > 420) alarms |= AlarmOverheat;

    tel_.heater_W = P_src_;
    tel_.airflow_kg_s = flow_;
    tel_.inlet_C = T_in_;
    tel_.exhaust_C = T_air_;
    tel_.air_C = T_air_;
    tel_.wall_C = T_drum_;
    tel_.element_C = (P.heat_source == "gas") ? T_in_ : T_outer_;
    tel_.drum_rpm = rpm;
    tel_.angle_rad = angle_;
    tel_.alarms = alarms;
}

double DrumMachine::probeEnv(ProbeLocation loc, const BeanBatch* beans, double& immersion) const {
    immersion = 0;
    switch (loc) {
        case ProbeLocation::BeanMass:
            if (beans && !beans->empty()) {
                immersion = tel_.bt_immersion;
                return immersion * beans->meanSurfaceT() + (1.0 - immersion) * T_air_;
            }
            return T_air_;
        case ProbeLocation::InletAir: return T_in_;
        case ProbeLocation::ExhaustAir:
        case ProbeLocation::ChamberAir: return T_air_;
        case ProbeLocation::Wall: return T_drum_;
        case ProbeLocation::Element: return tel_.element_C;
        case ProbeLocation::Ambient: return ambient_;
    }
    return T_air_;
}

}  // namespace roastsim
