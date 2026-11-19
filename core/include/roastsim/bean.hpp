// Green coffee definitions and the bean-parcel population model.
//
// A batch is represented by N parcels (default 200). Each parcel stands for a
// group of near-identical beans and carries its own two-node thermal state
// (surface + core), moisture, reaction progress, crack state, colour and
// defect accumulators. Machines compute the heat flowing into each parcel's
// surface; the batch integrates the bean physics. The parcels double as the
// data source for the future 3D view (one rendered bean samples one parcel).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "roastsim/json.hpp"
#include "roastsim/rng.hpp"
#include "roastsim/types.hpp"

namespace roastsim {

// Tunable physical/chemical constants. Defaults are hand-calibrated so typical
// profiles land in realistic ranges; any field can be overridden per bean in
// its JSON file under "kinetics".
struct Kinetics {
    // Drying, Schwartzberg-style: dX/dt = -A (X - Xeq)^2 / d_mm^2 * exp(-B / T_K)
    double dry_A = 2.0e10;
    double dry_B = 9889.0;
    double X_eq = 0.004;              // dry-basis floor
    double latent_J_kg = 2.5e6;       // vaporisation + desorption of bound water
    // Heat capacity of dry bean matter: a + b*T(C)  [J/(kg K)]
    double cp_dry_a = 1100.0;
    double cp_dry_b = 3.5;
    double k_bean = 0.20;             // W/(m K) effective conductivity
    // Bean density (BeanSpec::density, free-settled g/L) relative to density_ref.
    // Denser, harder beans hold water more tightly and conduct heat to the core
    // more slowly, so they dry later and crack later. Bean *size* is computed at
    // density_ref so density never makes a bean smaller (and faster) by accident.
    double density_ref = 700.0;
    double density_dry_exp = 1.0;     // drying rate x (ref/density)^exp
    double density_cond_exp = 3.0;    // surface-core conductance x (ref/density)^exp
    double surface_frac = 0.35;       // mass fraction in the surface node
    double shape_area = 1.20;         // area relative to an equal-volume sphere
    // Exothermic pyrolysis: dxi/dt = k0 exp(-Ea/RT) (1 - xi); heat = H * m_dry0 * dxi
    double exo_H = 60e3;              // J per kg dry matter
    double exo_k0 = 1.06e10;
    double exo_EaR = 14000.0;
    // Development (browning) integral, seconds-equivalent at dev_Tref
    double dev_EaR = 10000.0;
    double dev_Tref = 200.0;
    // Early yellowing integral, seconds-equivalent at brown_Tref
    double brown_EaR = 9000.0;
    double brown_Tref = 150.0;
    double brown_full = 150.0;        // eq-seconds for fully yellow
    // Agtron (gourmet scale) from development: min + span * exp(-D / tau)
    double agtron_min = 18.0;
    double agtron_span = 105.0;
    double agtron_tau = 196.0;
    // Dry-matter (organic) loss: max * (1 - exp(-D / tau))
    double organic_loss_max = 0.10;
    double organic_loss_tau = 400.0;
    // Cracks
    double crack_sigma = 1.6;         // C spread of thresholds inside a parcel
    double crack_energy_J = 0.25;     // endothermic flash per cracking bean
    double fc_pops_per_bean = 1.4;
    double sc_pops_per_bean = 2.5;
    // Expansion (diameter growth)
    double expansion_dev = 0.03;
    double expansion_fc = 0.14;
    double expansion_sc = 0.05;
    // Defects
    double scorch_T = 240.0;          // C, contact hot-spot temperature that burns
    double tipping_dT = 38.0;         // C surface-core gradient that burns tips while wet

    static Kinetics fromJson(const json::Value& v, const Kinetics& base);
};

struct BeanSpec {
    std::string id = "generic";
    std::string name = "Generic washed arabica";
    std::string origin;
    std::string process = "washed";
    std::string notes;
    double moisture_wb = 0.110;      // wet basis fraction
    double moisture_sd = 0.004;
    double density = 700.0;          // free-settled density, g/L (= kg/m^3) from spec sheets
    double bean_mass_mg = 150.0;
    double size_rel_sd = 0.10;       // relative sd of single-bean mass
    double fc_temp = 196.0;          // core temperature at first crack (mean)
    double fc_sd = 1.8;              // between-parcel spread
    double sc_temp = 224.0;
    double sc_sd = 2.2;
    double chaff_frac = 0.010;       // silverskin mass fraction
    double kinetics_scale = 1.0;     // >1 browns faster (e.g. naturals)
    double green_rgb[3] = {0.46, 0.52, 0.34};
    double bulk_density = 700.0;     // kg/m^3 of the green pile; JSON default = density
    Kinetics kinetics;

    static BeanSpec fromJson(const json::Value& v);
    json::Value toJson() const;
};

struct Parcel {
    // static
    double count0 = 0;       // beans represented at charge
    double count = 0;        // beans still present (ejection can remove some)
    double m_dry0 = 0;       // kg dry matter per bean at charge
    double X0 = 0;           // initial moisture (dry basis)
    double d0 = 0;           // m equivalent diameter at charge
    double fc_thr = 196, sc_thr = 224;
    double bias = 0;         // persistent exposure bias (position, shape)
    double chaff0 = 0;       // kg chaff per bean at charge
    // dynamic
    double m_dry = 0, X = 0;
    double T_s = 20, T_c = 20;
    double d = 0;
    double dev_s = 0, dev_c = 0, brown = 0, exo = 0;
    double fc_frac = 0, sc_frac = 0;
    double scorch = 0, tipping = 0;
    double oil = 0;
    double chaff = 0;
    double z_conv = 0, z_contact = 0;  // Ornstein-Uhlenbeck mixing states
    // per-step inputs from the machine (per bean)
    double q_in = 0;         // W into surface
    double spot_T = 0;       // hottest local surface temperature, C

    double area(const Kinetics& k) const { return k.shape_area * kPi * d * d; }
    double beanMass() const { return m_dry * (1.0 + X) + chaff; }
    double meanT(const Kinetics& k) const { return k.surface_frac * T_s + (1 - k.surface_frac) * T_c; }
    double devMean(const Kinetics& k) const { return k.surface_frac * dev_s + (1 - k.surface_frac) * dev_c; }
};

struct BeanSummary {
    bool present = false;
    double beans = 0;
    double mass_g = 0;
    double green_mass_g = 0;
    double weight_loss_pct = 0;
    double moisture_pct = 0;         // wet basis
    double T_surface = 0, T_core = 0, T_mean = 0;
    double T_min = 0, T_max = 0;
    double agtron_whole = 0, agtron_ground = 0, agtron_sd = 0;
    double dev_mean = 0;
    double fc_frac = 0, sc_frac = 0;
    double oil = 0;
    double scorch_frac = 0, tipping_frac = 0;
    double chaff_g = 0;
    double expansion = 1.0;          // mean diameter / initial
    double rgb[3] = {0, 0, 0};
};

struct BatchStepResult {
    int pops_fc = 0;
    int pops_sc = 0;
    double water_evap_kg = 0;
    double organic_loss_kg = 0;
    double chaff_released_kg = 0;
    double exo_W = 0;
};

class BeanBatch {
public:
    void init(const BeanSpec& spec, double batch_kg, int n_parcels, double T0, Rng& rng);
    void clear() { parcels_.clear(); }
    bool empty() const { return parcels_.empty(); }

    std::vector<Parcel>& parcels() { return parcels_; }
    const std::vector<Parcel>& parcels() const { return parcels_; }
    const Kinetics& kinetics() const { return k_; }
    const BeanSpec& spec() const { return spec_; }

    // Advance the mixing (exposure) random processes.
    void updateMixing(double dt, double tau_mix, Rng& rng);
    // Integrate bean physics given parcel.q_in / spot_T set by the machine.
    BatchStepResult integrate(double dt, Rng& rng);
    // Remove a fraction of beans (ejected from a fluid bed).
    void removeFraction(double frac);

    double totalMass() const;      // kg
    double totalBeans() const;
    double greenMass() const { return green_kg_; }
    double meanSurfaceT() const;   // count weighted
    double meanT() const;
    double totalArea() const;      // m^2
    double bulkVolume() const;     // m^3 (pile volume, expands while roasting)

    BeanSummary summary() const;

    double agtronFromDev(double dev) const;
    static void colorFor(const Parcel& p, const BeanSpec& spec, const Kinetics& k, double rgb[3]);

private:
    BeanSpec spec_;
    Kinetics k_;
    std::vector<Parcel> parcels_;
    double green_kg_ = 0;
};

}  // namespace roastsim
