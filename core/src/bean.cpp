#include "roastsim/bean.hpp"

#include <cmath>

namespace roastsim {

namespace {

double normalCdf(double x) { return 0.5 * std::erfc(-x / std::sqrt(2.0)); }

double arrhenius(double EaR, double Tref_C, double T_C) {
    // Rate relative to the rate at Tref. Clamp the temperature so cold beans
    // don't underflow and absurd temperatures don't overflow.
    double T = clampd(T_C, -50.0, 400.0);
    return std::exp(EaR * (1.0 / toK(Tref_C) - 1.0 / toK(T)));
}

void readColor(const json::Value& v, double rgb[3]) {
    if (v.isArray() && v.size() == 3)
        for (int i = 0; i < 3; ++i) rgb[i] = v[static_cast<std::size_t>(i)].num(rgb[i]);
}

}  // namespace

Kinetics Kinetics::fromJson(const json::Value& v, const Kinetics& base) {
    Kinetics k = base;
    if (!v.isObject()) return k;
#define RS_K(name) k.name = v.get(#name, k.name)
    RS_K(dry_A); RS_K(dry_B); RS_K(X_eq); RS_K(latent_J_kg); RS_K(cp_dry_a); RS_K(cp_dry_b);
    RS_K(k_bean); RS_K(density_ref); RS_K(density_dry_exp); RS_K(density_cond_exp); RS_K(surface_frac); RS_K(shape_area); RS_K(exo_H); RS_K(exo_k0); RS_K(exo_EaR);
    RS_K(dev_EaR); RS_K(dev_Tref); RS_K(brown_EaR); RS_K(brown_Tref); RS_K(brown_full);
    RS_K(agtron_min); RS_K(agtron_span); RS_K(agtron_tau); RS_K(organic_loss_max);
    RS_K(organic_loss_tau); RS_K(crack_sigma); RS_K(crack_energy_J); RS_K(fc_pops_per_bean);
    RS_K(sc_pops_per_bean); RS_K(expansion_dev); RS_K(expansion_fc); RS_K(expansion_sc);
    RS_K(scorch_T); RS_K(tipping_dT);
#undef RS_K
    return k;
}

BeanSpec BeanSpec::fromJson(const json::Value& v) {
    BeanSpec s;
    s.id = v.get("id", s.id.c_str());
    s.name = v.get("name", s.name.c_str());
    s.origin = v.get("origin", "");
    s.process = v.get("process", s.process.c_str());
    s.notes = v.get("notes", "");
    // Moisture may be given in percent for convenience.
    double m = v.get("moisture_pct", s.moisture_wb * 100.0);
    s.moisture_wb = v.get("moisture_wb", m / 100.0);
    s.moisture_sd = v.get("moisture_sd", s.moisture_sd);
    s.density = v.get("density", s.density);
    s.bean_mass_mg = v.get("bean_mass_mg", s.bean_mass_mg);
    s.size_rel_sd = v.get("size_rel_sd", s.size_rel_sd);
    s.fc_temp = v.get("fc_temp", s.fc_temp);
    s.fc_sd = v.get("fc_sd", s.fc_sd);
    s.sc_temp = v.get("sc_temp", s.sc_temp);
    s.sc_sd = v.get("sc_sd", s.sc_sd);
    s.chaff_frac = v.get("chaff_frac", s.chaff_frac);
    s.kinetics_scale = v.get("kinetics_scale", s.kinetics_scale);
    s.crack_loudness = v.get("crack_loudness", s.crack_loudness);
    s.bulk_density = v.get("bulk_density", s.density);  // free-settled density is the pile density
    readColor(v["green_rgb"], s.green_rgb);
    s.kinetics = Kinetics::fromJson(v["kinetics"], Kinetics{});
    return s;
}

json::Value BeanSpec::toJson() const {
    json::Value v = json::Value::object();
    v["id"] = id;
    v["name"] = name;
    v["origin"] = origin;
    v["process"] = process;
    v["notes"] = notes;
    v["moisture_pct"] = moisture_wb * 100.0;
    v["density"] = density;
    v["bean_mass_mg"] = bean_mass_mg;
    v["fc_temp"] = fc_temp;
    v["sc_temp"] = sc_temp;
    v["chaff_frac"] = chaff_frac;
    v["kinetics_scale"] = kinetics_scale;
    v["crack_loudness"] = crack_loudness;
    json::Value c = json::Value::array();
    for (double x : green_rgb) c.push(x);
    v["green_rgb"] = c;
    return v;
}

// ---------------------------------------------------------------- batch

void BeanBatch::init(const BeanSpec& spec, double batch_kg, int n_parcels, double T0, Rng& rng) {
    spec_ = spec;
    k_ = spec.kinetics;
    parcels_.clear();
    green_kg_ = batch_kg;
    if (batch_kg <= 0 || n_parcels <= 0) return;

    const double beanMass = spec.bean_mass_mg * 1e-6;  // kg
    const double totalBeans = batch_kg / beanMass;
    // Never let a parcel represent less than one bean.
    n_parcels = std::max(1, std::min(n_parcels, static_cast<int>(totalBeans)));
    parcels_.resize(static_cast<std::size_t>(n_parcels));

    // Sample relative bean masses, then normalise so the batch mass is exact.
    std::vector<double> rel(parcels_.size());
    double relSum = 0;
    for (auto& r : rel) {
        r = std::max(0.4, 1.0 + spec.size_rel_sd * rng.normal());
        relSum += r;
    }
    const double perParcelBeans = totalBeans / n_parcels;
    for (std::size_t i = 0; i < parcels_.size(); ++i) {
        Parcel& p = parcels_[i];
        double m_bean = beanMass * rel[i] * (static_cast<double>(n_parcels) / relSum);
        double mwb = clampd(spec.moisture_wb + spec.moisture_sd * rng.normal(), 0.04, 0.2);
        p.count0 = p.count = perParcelBeans;
        p.X0 = p.X = mwb / (1.0 - mwb);
        double chaffBean = m_bean * spec.chaff_frac;
        p.chaff0 = p.chaff = chaffBean;
        p.m_dry0 = p.m_dry = (m_bean - chaffBean) * (1.0 - mwb);
        double vol = m_bean / k_.density_ref;  // size from mass only (see Kinetics::density_ref)
        p.d0 = p.d = std::cbrt(6.0 * vol / kPi);
        p.fc_thr = spec.fc_temp + spec.fc_sd * rng.normal();
        p.sc_thr = spec.sc_temp + spec.sc_sd * rng.normal();
        // Wetter beans crack a touch later, smaller beans a touch earlier.
        p.fc_thr += (mwb - spec.moisture_wb) * 60.0 + (rel[i] - 1.0) * 2.0;
        p.bias = rng.normal();
        p.T_s = p.T_c = T0;
        p.z_conv = rng.normal();
        p.z_contact = rng.normal();
        p.spot_T = T0;
    }
}

void BeanBatch::updateMixing(double dt, double tau_mix, Rng& rng) {
    // Ornstein-Uhlenbeck with unit stationary variance and correlation time tau_mix.
    tau_mix = std::max(0.05, tau_mix);
    const double a = std::exp(-dt / tau_mix);
    const double b = std::sqrt(std::max(0.0, 1.0 - a * a));
    for (auto& p : parcels_) {
        p.z_conv = a * p.z_conv + b * rng.normal();
        p.z_contact = a * p.z_contact + b * rng.normal();
    }
}

void BeanBatch::removeFraction(double frac) {
    frac = clamp01(frac);
    for (auto& p : parcels_) p.count *= (1.0 - frac);
}

BatchStepResult BeanBatch::integrate(double dt, Rng& rng) {
    BatchStepResult res;
    const Kinetics& k = k_;
    const double phiS = k.surface_frac, phiC = 1.0 - k.surface_frac;
    const double kin = spec_.kinetics_scale;
    const double rhoRel = spec_.density > 0 ? k.density_ref / spec_.density : 1.0;
    const double dryF = std::pow(rhoRel, k.density_dry_exp);
    const double condF = std::pow(rhoRel, k.density_cond_exp);

    for (auto& p : parcels_) {
        if (p.count <= 0) continue;
        const double Tm = phiS * p.T_s + phiC * p.T_c;
        const double TmK = toK(Tm);

        // --- drying
        double Xex = std::max(0.0, p.X - k.X_eq);
        double dmm = p.d0 * 1000.0;
        double dXdt = -k.dry_A * Xex * Xex / (dmm * dmm) * std::exp(-k.dry_B / TmK) * dryF;
        double dX = std::max(dXdt * dt, -Xex);
        double evapKg = -dX * p.m_dry;               // per bean
        double Qevap = k.latent_J_kg * evapKg / dt;  // W per bean

        // --- exotherm
        double kexo = k.exo_k0 * std::exp(-k.exo_EaR / TmK);
        double dxi = (1.0 - p.exo) * (1.0 - std::exp(-kexo * dt));
        double Qexo = k.exo_H * p.m_dry0 * dxi / dt;

        // --- cracks (fraction of beans in this parcel past threshold)
        double fcT = normalCdf((p.T_c - p.fc_thr) / k.crack_sigma);
        double scT = normalCdf((p.T_c - p.sc_thr) / k.crack_sigma);
        double dfc = std::max(0.0, fcT - p.fc_frac);
        double dsc = std::max(0.0, scT - p.sc_frac);
        // Each pop carries what it sounds like: the bean's size, hardness and how
        // much steam is left to burst it (see docs/SOUND.md).
        const double hardness = spec_.density > 0 ? spec_.density / k.density_ref : 1.0;
        const double sizeMm = p.d * 1000.0;
        if (dfc > 0) {
            p.fc_frac += dfc;
            int n = rng.poisson(dfc * p.count * k.fc_pops_per_bean);
            res.pops_fc += n;
            if (n > 0) {
                CrackPop c;
                c.second = false;
                c.count = n;
                c.size_mm = static_cast<float>(sizeMm);
                c.hardness = static_cast<float>(hardness);
                c.moisture = static_cast<float>(p.X);
                c.loudness = static_cast<float>(spec_.crack_loudness * std::pow(hardness, 1.5) *
                                                clampd(std::sqrt(p.X / 0.06), 0.5, 1.4) * std::sqrt(sizeMm / 7.5));
                res.cracks.push_back(c);
            }
        }
        if (dsc > 0) {
            p.sc_frac += dsc;
            int n = rng.poisson(dsc * p.count * k.sc_pops_per_bean);
            res.pops_sc += n;
            if (n > 0) {
                CrackPop c;
                c.second = true;
                c.count = n;
                c.size_mm = static_cast<float>(sizeMm);
                c.hardness = static_cast<float>(hardness);
                c.moisture = static_cast<float>(p.X);
                c.loudness = static_cast<float>(spec_.crack_loudness * hardness * 0.5);
                res.cracks.push_back(c);
            }
        }
        double Qcrack = k.crack_energy_J * (dfc + 0.5 * dsc) / dt;

        // --- heat balance (two nodes)
        double cpS = k.cp_dry_a + k.cp_dry_b * p.T_s;
        double cpC = k.cp_dry_a + k.cp_dry_b * p.T_c;
        double Cs = phiS * p.m_dry * (cpS + p.X * kWaterCp);
        double Cc = phiC * p.m_dry * (cpC + p.X * kWaterCp);
        double G = 7.0 * k.k_bean * p.d * condF;  // W/K surface<->core
        double Qint = G * (p.T_s - p.T_c);
        double Qsink = Qevap + Qcrack - Qexo;
        double dTs = (p.q_in - Qint - phiS * Qsink) / Cs * dt;
        double dTc = (Qint - phiC * Qsink) / Cc * dt;
        p.T_s += dTs;
        p.T_c += dTc;

        p.X += dX;
        p.exo += dxi;

        // --- development integrals
        p.dev_s += kin * arrhenius(k.dev_EaR, k.dev_Tref, p.T_s) * dt;
        p.dev_c += kin * arrhenius(k.dev_EaR, k.dev_Tref, p.T_c) * dt;
        p.brown += kin * arrhenius(k.brown_EaR, k.brown_Tref, Tm) * dt;

        // --- organic loss
        double devMean = phiS * p.dev_s + phiC * p.dev_c;
        double mdOld = p.m_dry;
        p.m_dry = p.m_dry0 * (1.0 - k.organic_loss_max * (1.0 - std::exp(-devMean / k.organic_loss_tau)));
        double orgKg = std::max(0.0, mdOld - p.m_dry);

        // --- expansion, oil, chaff
        p.d = p.d0 * (1.0 + k.expansion_dev * std::min(1.0, devMean / 300.0) + k.expansion_fc * p.fc_frac +
                      k.expansion_sc * p.sc_frac);
        p.oil = std::max(p.oil, std::max(smoothstep(0.15, 0.9, p.sc_frac) * 0.8, smoothstep(420, 700, p.dev_s)));
        double dryProg = clamp01(1.0 - (p.X - 0.02) / std::max(1e-6, p.X0 - 0.02));
        double chaffTarget = p.chaff0 * (1.0 - 0.45 * dryProg - 0.5 * p.fc_frac);
        double chaffRel = 0;
        if (p.chaff > chaffTarget) {
            chaffRel = p.chaff - chaffTarget;
            p.chaff = chaffTarget;
        }

        // --- defects
        if (p.spot_T > k.scorch_T) p.scorch += dt * (p.spot_T - k.scorch_T) / (30.0 * 60.0);
        double grad = p.T_s - p.T_c;
        if (p.X > 0.06 && grad > k.tipping_dT) p.tipping += dt * (grad - k.tipping_dT) / (10.0 * 60.0);

        res.water_evap_kg += evapKg * p.count;
        res.organic_loss_kg += orgKg * p.count;
        res.chaff_released_kg += chaffRel * p.count;
        res.exo_W += Qexo * p.count;
    }
    return res;
}

double BeanBatch::totalMass() const {
    double m = 0;
    for (const auto& p : parcels_) m += p.count * p.beanMass();
    return m;
}

double BeanBatch::totalBeans() const {
    double n = 0;
    for (const auto& p : parcels_) n += p.count;
    return n;
}

double BeanBatch::meanSurfaceT() const {
    double s = 0, n = 0;
    for (const auto& p : parcels_) { s += p.count * p.T_s; n += p.count; }
    return n > 0 ? s / n : 0;
}

double BeanBatch::meanT() const {
    double s = 0, n = 0;
    for (const auto& p : parcels_) { s += p.count * p.meanT(k_); n += p.count; }
    return n > 0 ? s / n : 0;
}

double BeanBatch::totalArea() const {
    double a = 0;
    for (const auto& p : parcels_) a += p.count * p.area(k_);
    return a;
}

double BeanBatch::bulkVolume() const {
    double v = 0;
    for (const auto& p : parcels_) {
        double vol0 = p.count * (p.m_dry0 * (1 + p.X0) + p.chaff0) / spec_.bulk_density;
        double e = p.d / p.d0;
        v += vol0 * e * e * e;
    }
    return v;
}

double BeanBatch::agtronFromDev(double dev) const {
    return k_.agtron_min + k_.agtron_span * std::exp(-std::max(0.0, dev) / k_.agtron_tau);
}

void BeanBatch::colorFor(const Parcel& p, const BeanSpec& spec, const Kinetics& k, double rgb[3]) {
    // Stage 1: green -> straw/yellow, driven by the low-temperature browning integral.
    static const double yellow[3] = {0.80, 0.70, 0.44};
    double y = smoothstep(0.0, 1.0, p.brown / k.brown_full);
    double base[3];
    for (int i = 0; i < 3; ++i) base[i] = lerp(spec.green_rgb[i], yellow[i], y);
    // Stage 2: Agtron-indexed browns from the surface development integral.
    struct Stop { double agtron; double c[3]; };
    static const Stop stops[] = {
        {125, {0.80, 0.68, 0.44}}, {100, {0.72, 0.52, 0.31}}, {85, {0.62, 0.42, 0.25}},
        {70, {0.51, 0.32, 0.19}},  {58, {0.42, 0.26, 0.15}},  {48, {0.34, 0.21, 0.12}},
        {38, {0.25, 0.15, 0.09}},  {28, {0.16, 0.10, 0.07}},  {18, {0.08, 0.06, 0.05}},
    };
    double ag = k.agtron_min + k.agtron_span * std::exp(-p.dev_s / k.agtron_tau);
    double brownC[3] = {stops[0].c[0], stops[0].c[1], stops[0].c[2]};
    const int n = static_cast<int>(sizeof stops / sizeof stops[0]);
    for (int i = 0; i + 1 < n; ++i) {
        if (ag <= stops[i].agtron && ag >= stops[i + 1].agtron) {
            double t = (stops[i].agtron - ag) / (stops[i].agtron - stops[i + 1].agtron);
            for (int c = 0; c < 3; ++c) brownC[c] = lerp(stops[i].c[c], stops[i + 1].c[c], t);
            break;
        }
        if (i + 2 == n && ag < stops[n - 1].agtron)
            for (int c = 0; c < 3; ++c) brownC[c] = stops[n - 1].c[c];
    }
    double w = smoothstep(5.0, 60.0, p.dev_s);
    for (int c = 0; c < 3; ++c) rgb[c] = lerp(base[c], brownC[c], w);
    // Scorch marks darken the surface.
    double s = clamp01(p.scorch) * 0.5;
    for (int c = 0; c < 3; ++c) rgb[c] *= (1.0 - s);
}

BeanSummary BeanBatch::summary() const {
    BeanSummary s;
    if (parcels_.empty()) return s;
    s.present = true;
    double n = 0, ts = 0, tc = 0, tm = 0, water = 0, dry = 0, chaff = 0, agW = 0, agG = 0, agG2 = 0;
    double fc = 0, sc = 0, oil = 0, scorch = 0, tip = 0, exp = 0, dev = 0, greenLeft = 0;
    double rgb[3] = {0, 0, 0};
    s.T_min = 1e9;
    s.T_max = -1e9;
    for (const auto& p : parcels_) {
        double c = p.count;
        if (c <= 0) continue;
        n += c;
        ts += c * p.T_s;
        tc += c * p.T_c;
        double pm = p.meanT(k_);
        tm += c * pm;
        s.T_min = std::min(s.T_min, pm);
        s.T_max = std::max(s.T_max, pm);
        greenLeft += c * (p.m_dry0 * (1.0 + p.X0) + p.chaff0);
        water += c * p.m_dry * p.X;
        dry += c * p.m_dry;
        chaff += c * p.chaff;
        double aw = agtronFromDev(p.dev_s);
        double ag = agtronFromDev(p.devMean(k_));
        agW += c * aw;
        agG += c * ag;
        agG2 += c * ag * ag;
        dev += c * p.devMean(k_);
        fc += c * p.fc_frac;
        sc += c * p.sc_frac;
        oil += c * p.oil;
        if (p.scorch > 0.35) scorch += c;
        if (p.tipping > 0.35) tip += c;
        exp += c * (p.d / p.d0);
        double col[3];
        colorFor(p, spec_, k_, col);
        for (int i = 0; i < 3; ++i) rgb[i] += c * col[i];
    }
    if (n <= 0) return s;
    s.beans = n;
    s.mass_g = (water + dry + chaff) * 1000.0;
    s.green_mass_g = green_kg_ * 1000.0;
    // Loss relative to the green weight of the beans still present (ejected beans
    // are reported separately, not as roast loss).
    s.weight_loss_pct = greenLeft > 0 ? 100.0 * (1.0 - s.mass_g / 1000.0 / greenLeft) : 0;
    s.moisture_pct = 100.0 * water / std::max(1e-12, water + dry);
    s.T_surface = ts / n;
    s.T_core = tc / n;
    s.T_mean = tm / n;
    s.agtron_whole = agW / n;
    s.agtron_ground = agG / n;
    s.agtron_sd = std::sqrt(std::max(0.0, agG2 / n - s.agtron_ground * s.agtron_ground));
    s.dev_mean = dev / n;
    s.fc_frac = fc / n;
    s.sc_frac = sc / n;
    s.oil = oil / n;
    s.scorch_frac = scorch / n;
    s.tipping_frac = tip / n;
    s.chaff_g = chaff * 1000.0;
    s.expansion = exp / n;
    for (int i = 0; i < 3; ++i) s.rgb[i] = rgb[i] / n;
    return s;
}

}  // namespace roastsim
