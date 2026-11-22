#include "roastsim/simulation.hpp"

#include <cmath>

namespace roastsim {

namespace {

enum Mark { MCharge, MTp, MDry, MFcs, MFce, MScs, MSce, MDrop, MCool, MCount };

// Auto-detection thresholds, shared with the time-to-milestone estimates.
constexpr double kDryMeanC = 150.0;    // DRY: bean mass mean temperature
constexpr double kFcStartFrac = 0.02;  // FCs: share of beans that have cracked

double normalCdf(double x) { return 0.5 * std::erfc(-x / std::sqrt(2.0)); }

int markIndex(const std::string& t) {
    static const char* names[] = {"CHARGE", "TP", "DRY", "FCs", "FCe", "SCs", "SCe", "DROP", "COOL"};
    for (int i = 0; i < MCount; ++i)
        if (t == names[i]) return i;
    return -1;
}

struct AlarmName { std::uint32_t bit; const char* name; };
const AlarmName kAlarmNames[] = {
    {AlarmThermostatOpen, "thermostat_open"}, {AlarmFuseBlown, "fuse_blown"},
    {AlarmBeansEjecting, "beans_ejecting"},   {AlarmBedStalled, "bed_stalled"},
    {AlarmScorching, "scorching"},            {AlarmSmokeHeavy, "smoke_heavy"},
    {AlarmFireRisk, "fire_risk"},             {AlarmOverheat, "overheat"},
    {AlarmCentrifuging, "centrifuging"},      {AlarmProbeFault, "probe_fault"},
};

}  // namespace

const char* roastLevelName(double ag) {
    if (ag >= 90) return "Very light (Cinnamon)";
    if (ag >= 78) return "Light (New England)";
    if (ag >= 68) return "City";
    if (ag >= 60) return "City+";
    if (ag >= 52) return "Full City";
    if (ag >= 44) return "Full City+";
    if (ag >= 36) return "Vienna";
    if (ag >= 28) return "French";
    return "Italian / charred";
}

Simulation::Simulation() = default;
Simulation::~Simulation() = default;

void Simulation::setSeed(std::uint64_t seed) { rng_.reseed(seed); }

void Simulation::load(const RoasterDef& roaster, const BeanSpec& bean) {
    Environment env = env_;
    env.line_voltage = roaster.nominalLineVoltage();
    load(roaster, bean, env);
}

void Simulation::load(const RoasterDef& roaster, const BeanSpec& bean, const Environment& env) {
    machine_.reset();
    roaster_ = roaster;
    bean_ = bean;
    env_ = env;
    machine_ = createMachine(roaster_);
    probes_.clear();
    for (const auto& pd : roaster_.probes) probes_.emplace_back(pd);
    coldReset();  // switched off, controls at the preset's defaults
}

void Simulation::setBean(const BeanSpec& bean) { bean_ = bean; }

void Simulation::setPower(bool on, const std::string& origin) {
    if (powered_ == on) return;
    powered_ = on;
    addEvent("NOTE", origin.empty() ? "user" : origin, on ? "power on" : "power off");
}

void Simulation::setControl(Control c, double pct) {
    const ControlDef* cd = roaster_.control(c);
    if (!cd) return;
    double v = clampd(pct, cd->min, cd->max);
    if (cd->toggle) v = v >= 50 ? cd->max : cd->min;
    target_[static_cast<int>(c)] = cd->snapToLevel(v);
}

int Simulation::probeIndex(const char* id) const { return roaster_.probeIndex(id); }

void Simulation::addEvent(const std::string& type, const std::string& origin, const std::string& detail) {
    SimEvent e;
    e.seq = ++seq_;
    e.type = type;
    e.detail = detail;
    e.origin = origin;
    e.t_session = t_;
    e.t_roast = chargeT_ >= 0 ? t_ - chargeT_ : -1;
    e.powered = powered_;
    int bi = probeIndex("BT");
    e.bt = bi >= 0 ? probes_[static_cast<std::size_t>(bi)].read() : 0;
    events_.push_back(e);
    if (events_.size() > 2000) events_.erase(events_.begin(), events_.begin() + 500);
    int m = markIndex(type);
    if (m >= 0) marked_[m] = true;
}

std::vector<CrackEvent> Simulation::cracksSince(std::uint64_t seq, std::size_t max) const {
    std::vector<CrackEvent> out;
    // cracks_ is ordered by seq; skip the old ones quickly from the back.
    std::size_t i = cracks_.size();
    while (i > 0 && cracks_[i - 1].seq > seq) --i;
    for (; i < cracks_.size() && out.size() < max; ++i) out.push_back(cracks_[i]);
    return out;
}

std::vector<SimEvent> Simulation::eventsSince(std::uint64_t seq) const {
    std::vector<SimEvent> out;
    for (const auto& e : events_)
        if (e.seq > seq) out.push_back(e);
    return out;
}

bool Simulation::charge(double batch_g, const std::string& origin, std::string* err) {
    if (beanLoc_ == BeanLocation::Machine) {
        if (err) *err = "beans are already in the roaster";
        return false;
    }
    if (!(batch_g > 0)) {
        if (err) *err = "batch size must be positive";
        return false;
    }
    if (batch_g > roaster_.batch.max_g * 1.5) {
        if (err) *err = "batch far above this roaster's capacity";
        return false;
    }
    double T0 = roaster_.charge_bean_temp_C > -273 ? roaster_.charge_bean_temp_C : env_.ambient_C;
    beans_.init(bean_, batch_g / 1000.0, parcelCount_, T0, rng_);
    beanLoc_ = BeanLocation::Machine;
    batchG_ = batch_g;
    initialBeans_ = beans_.totalBeans();
    chargeT_ = t_;
    dropT_ = -1;
    phase_ = RoastPhase::Roasting;
    for (bool& m : marked_) m = false;
    prevRateT_ = -1;
    rateMeanT_ = rateCoreT_ = 0;
    etaDry_ = etaFc_ = -1;
    tpMin_ = 1e9;
    tpMinT_ = -1;
    popsFc_ = popsSc_ = 0;
    organicRate_ = 0;
    // Pre-charge history (preheat) stays, at negative roast times: front ends
    // may not have read it yet, e.g. when a preheat fast-forward is followed
    // straight away by a charge from MCP or a script.
    for (auto& h : history_) h.t_roast = static_cast<float>(h.t - t_);
    addEvent("CHARGE", origin);
    chargeSeq_ = seq_;
    return true;
}

bool Simulation::drop(const std::string& origin, std::string* err) {
    if (beanLoc_ != BeanLocation::Machine) {
        if (err) *err = "no beans in the roaster";
        return false;
    }
    beanLoc_ = BeanLocation::Tray;
    dropT_ = t_;
    phase_ = RoastPhase::Cooling;
    addEvent("DROP", origin);
    return true;
}

bool Simulation::coolEnd(const std::string& origin) {
    if (phase_ != RoastPhase::Cooling) return false;
    phase_ = RoastPhase::Done;
    addEvent("COOL", origin);
    return true;
}

void Simulation::markEvent(const std::string& type, const std::string& origin) {
    int m = markIndex(type);
    if (m < 0) {
        addEvent("NOTE", origin, type);
        return;
    }
    if (m == MCharge || m == MDrop || m == MCool) return;  // use the real commands
    if (phase_ != RoastPhase::Roasting || marked_[m]) return;
    addEvent(type, origin);
}

void Simulation::emptyMachine() {
    beans_.clear();
    beanLoc_ = BeanLocation::None;
    phase_ = RoastPhase::Idle;
    chargeT_ = dropT_ = -1;
}

void Simulation::coldReset() {
    emptyMachine();
    if (machine_) machine_->reset(env_);
    for (auto& p : probes_) p.reset(env_.ambient_C);
    btBuf_.clear();
    etBuf_.clear();
    rorBt_ = rorEt_ = 0;
    historyBase_ += history_.size();
    history_.clear();
    events_.clear();
    lastAlarms_ = 0;
    smoke_ = chaffRate_ = organicRate_ = 0;
    popsFc_ = popsSc_ = 0;
    cracks_.clear();
    nextHistory_ = nextRorSample_ = t_;
    // A cold machine is a switched-off one with its dials back at the preset's
    // defaults. Silent: the event log was just cleared, and the reset note
    // below (with power = off) tells front ends what happened.
    powered_ = false;
    for (int i = 0; i < kControlCount; ++i) target_[i] = actual_[i] = 0;
    for (const auto& cd : roaster_.controls) target_[static_cast<int>(cd.id)] = cd.snapToLevel(cd.def);
    // First entry of the fresh event log: front ends start a new graph on it.
    addEvent("NOTE", "sim", "cold reset");
}

void Simulation::repair() {
    if (machine_) machine_->repair();
    addEvent("NOTE", "user", "repaired");
}

bool Simulation::setProbeFault(const std::string& probeId, ProbeFault f, double param) {
    for (auto& p : probes_) {
        if (p.def().id == probeId) {
            p.setFault(f, param);
            return true;
        }
    }
    return false;
}

double Simulation::preheat(double target_C, double maxSeconds, bool* reachedOut) {
    if (reachedOut) *reachedOut = false;
    if (beanLoc_ == BeanLocation::Machine || !machine_) return -1;
    if (!powered_) return -1;  // a switched-off machine cannot heat
    Control ctl = roaster_.hasControl(Control::Heat) ? Control::Heat : Control::Voltage;
    const ControlDef* heater = roaster_.control(ctl);
    if (!heater) return -1;
    // Make sure air moves / drum turns so nothing overheats while we wait.
    for (Control c : {Control::Fan, Control::Drum, Control::Stir}) {
        const ControlDef* cd = roaster_.control(c);
        if (!cd) continue;
        const double defaultSetting = cd->snapToLevel(cd->def);
        if (target_[static_cast<int>(c)] < defaultSetting) target_[static_cast<int>(c)] = defaultSetting;
    }
    int ei = probeIndex("ET");
    if (ei < 0) return -1;
    double I = target_[static_cast<int>(ctl)] / 100.0;
    // Drums and pans store heat in metal: wait until the walls stop warming.
    // Air roasters have little of it, so a warm-up is done once the air holds
    // the target for a short while.
    const bool air = roaster_.kind == RoasterKind::FluidBed;
    const double holdFor = air ? 30.0 : 90.0;
    // Hot air follows the heater within seconds, so an air roaster's
    // controller can close the last few degrees ten times faster; it only
    // integrates while the heater is not flat out, so it does not overshoot.
    const double ki = air ? 0.004 : 0.0004;
    double stable = 0, used = 0, prevWall = machine_->telemetry().wall_C;
    double flatOut = 0, flatOutEt = 0;  // seconds at full heat, and ET when that run began
    bool reached = false;
    while (used < maxSeconds) {
        double et = probes_[static_cast<std::size_t>(ei)].trueTemp();
        double err = target_C - et;
        const double raw = 0.04 * err + I;
        if (!air || (raw > 0 && raw < 1)) I = clamp01(I + ki * err * kStep);
        double u = clamp01(0.04 * err + I);
        // An air roaster at full heat whose air has stopped warming will not
        // get any hotter: stop there instead of waiting out maxSeconds.
        if (air && u >= 0.999) {
            if (flatOut == 0) flatOutEt = et;
            flatOut += kStep;
            if (flatOut >= 30) {
                if (et - flatOutEt < 0.5) break;
                flatOut = 0;
            }
        } else {
            flatOut = 0;
        }
        target_[static_cast<int>(ctl)] = heater->snapToLevel(u * 100.0);
        stepOnce(kStep);
        used += kStep;
        double wall = machine_->telemetry().wall_C;
        double wallRate = std::fabs(wall - prevWall) / kStep;
        prevWall = wall;
        if (std::fabs(err) < 1.5 && (air || wallRate < 0.05)) stable += kStep;
        else stable = 0;
        if (stable > holdFor) {
            reached = true;
            break;
        }
    }
    if (heater->levels == 0) target_[static_cast<int>(ctl)] = std::round(target_[static_cast<int>(ctl)]);
    const double et = probes_[static_cast<std::size_t>(ei)].trueTemp();
    addEvent("NOTE", "preheat",
             reached ? "preheated to " + json::formatNumber(target_C, 1)
                     : "preheat stopped at ET " + json::formatNumber(et, 1) + " (target " +
                           json::formatNumber(target_C, 1) + ")");
    if (reachedOut) *reachedOut = reached;
    return used;
}

void Simulation::step(double dt) {
    // Integrate in fixed slices so results don't depend on the caller's frame
    // rate; the fractional remainder carries over to the next call.
    double total = dt + stepCarry_;
    int n = static_cast<int>(total / kStep);
    stepCarry_ = total - n * kStep;
    for (int i = 0; i < n; ++i) stepOnce(kStep);
}

void Simulation::stepTray(double dt) {
    const Kinetics& k = beans_.kinetics();
    bool on = actual_[static_cast<int>(Control::Cooling)] >= 50.0;
    double h = on ? roaster_.cool.h_on : roaster_.cool.h_off;
    beans_.updateMixing(dt, on ? 2.0 : 30.0, rng_);
    for (auto& p : beans_.parcels()) {
        double e = exposure(p, 0.2, 0.05);
        p.q_in = h * p.area(k) * e * (env_.ambient_C - p.T_s);
        p.spot_T = p.T_s;
    }
}

void Simulation::stepOnce(double dt) {
    if (!machine_) return;
    // ---- actuator slew (power off: everything winds down to 0; the dial
    // settings stay untouched and take effect again when switched on)
    for (const auto& cd : roaster_.controls) {
        int i = static_cast<int>(cd.id);
        double target = powered_ ? target_[i] : 0.0;
        if (cd.slew_per_s > 0) {
            double d = target - actual_[i];
            double m = cd.slew_per_s * dt;
            actual_[i] += clampd(d, -m, m);
        } else {
            actual_[i] = target;
        }
    }

    MachineInputs in;
    in.env = &env_;
    for (int i = 0; i < kControlCount; ++i) in.u[i] = clamp01(actual_[i] / 100.0);
    in.beans = (beanLoc_ == BeanLocation::Machine) ? &beans_ : nullptr;
    in.rng = &rng_;
    machine_->step(dt, in);

    BatchStepResult res;
    if (beanLoc_ == BeanLocation::Tray) stepTray(dt);
    if (beanLoc_ != BeanLocation::None) res = beans_.integrate(dt, rng_, env_);
    popsFc_ += static_cast<std::uint64_t>(res.pops_fc);
    popsSc_ += static_cast<std::uint64_t>(res.pops_sc);
    for (const CrackPop& c : res.cracks) {
        int n = std::min(c.count, 200);  // a huge batch at peak crack is a roar; cap the bookkeeping
        for (int i = 0; i < n; ++i) {
            CrackEvent e;
            e.seq = ++crackSeq_;
            e.t = t_ + rng_.uniform() * dt;
            e.second = c.second;
            e.size_mm = c.size_mm;
            e.hardness = c.hardness;
            e.moisture = c.moisture;
            e.loudness = c.loudness;
            e.where = beanLoc_;
            cracks_.push_back(e);
        }
    }
    while (cracks_.size() > 8192) cracks_.pop_front();

    double a = 1.0 - std::exp(-dt / 3.0);
    organicRate_ += (res.organic_loss_kg / dt - organicRate_) * a;
    chaffRate_ += (res.chaff_released_kg * 1000.0 / dt - chaffRate_) * a;
    double kg = std::max(0.05, batchG_ / 1000.0);
    double smokeTarget = clamp01(organicRate_ * 1000.0 * 60.0 / kg / 25.0);
    // Chaff left in a hot pan (stir roasters) smoulders.
    const MachineTelemetry& tel = machine_->telemetry();
    if (roaster_.kind == RoasterKind::StirPan && tel.wall_C > 260 && beanLoc_ == BeanLocation::Machine)
        smokeTarget = std::min(1.0, smokeTarget + 0.15 * smoothstep(260, 320, tel.wall_C));
    smoke_ += (smokeTarget - smoke_) * a;

    updateProbes(dt);
    t_ += dt;
    ++frame_;

    if (t_ >= nextRorSample_) {
        nextRorSample_ = t_ + 0.5;
        auto push = [&](std::deque<std::pair<double, double>>& buf, const char* id) {
            int i = probeIndex(id);
            if (i < 0) return;
            double v = probes_[static_cast<std::size_t>(i)].read();
            if (v == -1.0) return;
            buf.emplace_back(t_, v);
            while (!buf.empty() && buf.front().first < t_ - 30.0) buf.pop_front();
        };
        push(btBuf_, "BT");
        push(etBuf_, "ET");
        rorBt_ = rorFrom(btBuf_);
        rorEt_ = rorFrom(etBuf_);
        detectEvents();
    }
    if (t_ >= nextHistory_) {
        nextHistory_ = t_ + 1.0;
        sampleHistory();
    }
}

void Simulation::updateProbes(double dt) {
    const BeanBatch* inMachine = (beanLoc_ == BeanLocation::Machine) ? &beans_ : nullptr;
    for (auto& p : probes_) {
        double imm = 0;
        double Tenv = p.def().loc == ProbeLocation::Ambient ? env_.ambient_C
                                                           : machine_->probeEnv(p.def().loc, inMachine, imm);
        p.step(dt, Tenv, imm, rng_);
    }
}

double Simulation::rorFrom(const std::deque<std::pair<double, double>>& buf) const {
    // Least-squares slope over the window, in degrees per minute.
    if (buf.size() < 4) return 0;
    double n = 0, st = 0, sv = 0, stt = 0, stv = 0;
    double t0 = buf.front().first;
    for (const auto& s : buf) {
        double t = s.first - t0;
        n += 1;
        st += t;
        sv += s.second;
        stt += t * t;
        stv += t * s.second;
    }
    double den = n * stt - st * st;
    if (std::fabs(den) < 1e-9) return 0;
    return (n * stv - st * sv) / den * 60.0;
}

void Simulation::sampleHistory() {
    HistorySample h;
    h.t = static_cast<float>(t_);
    h.t_roast = static_cast<float>(chargeT_ >= 0 ? t_ - chargeT_ : -1);
    int bi = probeIndex("BT"), ei = probeIndex("ET");
    h.bt = static_cast<float>(probes_[static_cast<std::size_t>(bi)].read());
    h.et = static_cast<float>(probes_[static_cast<std::size_t>(ei)].read());
    h.ror_bt = static_cast<float>(rorBt_);
    h.ror_et = static_cast<float>(rorEt_);
    h.bean_true = static_cast<float>(beanLoc_ != BeanLocation::None ? beans_.meanT() : 0.0);
    h.heat = static_cast<float>(actual_[static_cast<int>(
        roaster_.hasControl(Control::Heat) ? Control::Heat : Control::Voltage)]);
    h.fan = static_cast<float>(actual_[static_cast<int>(Control::Fan)]);
    h.drum = static_cast<float>(actual_[static_cast<int>(
        roaster_.hasControl(Control::Drum) ? Control::Drum : Control::Stir)]);
    history_.push_back(h);
    // ~6 hours max
    while (history_.size() > 21600) {
        history_.pop_front();
        ++historyBase_;
    }
}

void Simulation::detectEvents() {
    // Alarms (machine + simulation level), edge-triggered into the event log.
    std::uint32_t alarms = machine_->telemetry().alarms;
    if (!powered_) alarms &= ~AlarmBedStalled;  // a switched-off machine is not "stalled"
    double scorchingFrac = 0, meanT = 0, coreT = 0, fc = 0, sc = 0, n = 0;
    if (beanLoc_ != BeanLocation::None) {
        const Kinetics& k = beans_.kinetics();
        for (const auto& p : beans_.parcels()) {
            n += p.count;
            meanT += p.count * p.meanT(k);
            coreT += p.count * p.T_c;
            fc += p.count * p.fc_frac;
            sc += p.count * p.sc_frac;
            if (p.scorchExcess(k) > 0) scorchingFrac += p.count;
        }
        if (n > 0) {
            meanT /= n;
            coreT /= n;
            fc /= n;
            sc /= n;
            scorchingFrac /= n;
        }
    }
    if (beanLoc_ == BeanLocation::Machine && scorchingFrac > 0.05) alarms |= AlarmScorching;
    if (smoke_ > 0.7) alarms |= AlarmSmokeHeavy;
    if (beanLoc_ == BeanLocation::Machine && meanT > 250) alarms |= AlarmFireRisk;
    for (const auto& p : probes_)
        if (p.fault() != ProbeFault::None) alarms |= AlarmProbeFault;
    // Latch alarms with a 10 s hold so a condition hovering around its
    // threshold produces one event, not a burst.
    for (int b = 0; b < 32; ++b) {
        std::uint32_t bit = 1u << b;
        if (alarms & bit) {
            alarmSeen_[b] = t_;
            if (!(lastAlarms_ & bit)) {
                lastAlarms_ |= bit;
                for (const auto& an : kAlarmNames)
                    if (an.bit == bit) addEvent("ALARM", "sim", an.name);
            }
        } else if ((lastAlarms_ & bit) && t_ - alarmSeen_[b] > 10.0) {
            lastAlarms_ &= ~bit;
        }
    }

    if (phase_ == RoastPhase::Roasting) {
        double tr = t_ - chargeT_;
        int bi = probeIndex("BT");
        double bt = probes_[static_cast<std::size_t>(bi)].read();
        if (!marked_[MTp] && bt != -1.0) {
            if (bt < tpMin_) {
                tpMin_ = bt;
                tpMinT_ = t_;
            } else if (tr > 15 && bt > tpMin_ + 2.0) {
                addEvent("TP", "sim");
                events_.back().t_session = tpMinT_;
                events_.back().t_roast = tpMinT_ - chargeT_;
                events_.back().bt = tpMin_;
            }
        }
        if (!marked_[MDry] && meanT >= kDryMeanC) addEvent("DRY", "sim");
        if (!marked_[MFcs] && fc >= kFcStartFrac) addEvent("FCs", "sim");
        if (marked_[MFcs] && !marked_[MFce] && fc >= 0.90) addEvent("FCe", "sim");
        if (!marked_[MScs] && sc >= 0.02) addEvent("SCs", "sim");
        if (marked_[MScs] && !marked_[MSce] && sc >= 0.85) addEvent("SCe", "sim");
    }
    estimateMilestones(meanT, coreT);
    if (phase_ == RoastPhase::Cooling) {
        if (meanT < 40.0 || t_ - dropT_ > 900) coolEnd("sim");
    }
}

// Projects the beans forward to estimate how long until auto-detection would
// call DRY and first crack. Beans heat towards their surroundings (Newton's
// law), so the rate of rise keeps falling as they near them: we project along
// that curve, levelling off at the temperature the machine is driving the
// beans towards (hot air, drum wall, pan and radiation weighted by how
// strongly each couples to the beans; ET if the machine can't say), with the
// time constant that matches the current heating rate (tau = gap / rate).
// A straight line would read far too short early in the roast. When the
// beans are already at or above that temperature (exotherm) the projection
// falls back to linear. Called twice a second.
namespace {
struct Projection {
    double rate = 0, gap = -1;  // gap = target - T; < 0: linear
    double ahead(double dt) const {
        if (gap <= 0) return rate * dt;
        double tau = gap / rate;
        return gap * (1.0 - std::exp(-dt / tau));
    }
};
Projection projection(double rate, double T, double et) {
    Projection p;
    p.rate = rate;
    if (et > T + 1.0 && rate > 0) p.gap = et - T;
    return p;
}
}  // namespace

void Simulation::estimateMilestones(double meanT, double coreT) {
    if (phase_ != RoastPhase::Roasting) return;
    etaDry_ = etaFc_ = -1;
    // Smoothed heating rates (C/s) over ~20 s; bean temperatures are
    // noise-free, so this is stable.
    if (prevRateT_ >= 0 && t_ > prevRateT_) {
        double dt = t_ - prevRateT_, a = std::min(1.0, dt / 20.0);
        rateMeanT_ += ((meanT - prevMeanT_) / dt - rateMeanT_) * a;
        rateCoreT_ += ((coreT - prevCoreT_) / dt - rateCoreT_) * a;
    }
    // What the beans are heading for: the machine's own figure, else ET
    // averaged over the last 10 s (the probe is noisy; -1 = unavailable).
    double et = machine_ ? machine_->telemetry().bean_env_C : -1;
    if (et < 0) {
        double etSum = 0;
        int etN = 0;
        for (const auto& s : etBuf_)
            if (s.first >= t_ - 10.0) { etSum += s.second; ++etN; }
        if (etN > 0) et = etSum / etN;
    }
    prevMeanT_ = meanT;
    prevCoreT_ = coreT;
    prevRateT_ = t_;
    if (t_ - chargeT_ < 20) return;  // rates not settled yet (and beans still cooling the machine)

    // From here on a pending milestone gets a time, or kStalled if the beans
    // won't get there at the current heat (within an hour, or barely heating).
    constexpr double kStalled = -2;
    constexpr double kMinRate = 0.5 / 60.0;  // below 0.5 C/min there is nothing useful to project
    constexpr double kHorizon = 3600.0;
    auto solve = [&](auto reached) {
        if (reached(0.0)) return 0.0;
        if (!reached(kHorizon)) return kStalled;
        double lo = 0, hi = kHorizon;
        for (int i = 0; i < 24; ++i) {
            double mid = 0.5 * (lo + hi);
            (reached(mid) ? hi : lo) = mid;
        }
        return hi;
    };
    if (!marked_[MDry]) {
        Projection pr = projection(rateMeanT_, meanT, et);
        etaDry_ = meanT < kDryMeanC && rateMeanT_ <= kMinRate
                      ? kStalled
                      : solve([&](double dt) { return meanT + pr.ahead(dt) >= kDryMeanC; });
    }
    // First crack only once drying is over: until then evaporation keeps
    // slowing the beans in a way no projection from the current rate can
    // see, and the estimate reads many minutes short.
    if (!marked_[MDry] || marked_[MFcs]) return;
    if (rateCoreT_ <= kMinRate) etaFc_ = kStalled;
    else {
        // Share of beans cracked once every core has warmed as projected for
        // `ahead` seconds, with each parcel's own crack threshold (bean.cpp).
        const Kinetics& k = beans_.kinetics();
        Projection pr = projection(rateCoreT_, coreT, et);
        auto cracked = [&](double ahead) {
            double rise = pr.ahead(ahead), f = 0, n = 0;
            for (const auto& p : beans_.parcels()) {
                n += p.count;
                double proj = normalCdf((p.T_c + rise - p.fc_thr) / k.crack_sigma);
                f += p.count * std::max(p.fc_frac, proj);
            }
            return n > 0 ? f / n : 0.0;
        };
        etaFc_ = solve([&](double dt) { return cracked(dt) >= kFcStartFrac; });
    }
}

Snapshot Simulation::snapshot() const {
    Snapshot s;
    s.t_session = t_;
    s.t_roast = chargeT_ >= 0 ? t_ - chargeT_ : -1;
    s.phase = phase_;
    s.bean_location = beanLoc_;
    s.roaster_id = roaster_.id;
    s.roaster_name = roaster_.name;
    s.bean_id = bean_.id;
    s.bean_name = bean_.name;
    s.kind = roaster_.kind;
    for (const auto& p : probes_) {
        ProbeReading r;
        r.id = p.def().id;
        r.loc = p.def().loc;
        r.value = p.read();
        r.fault = p.fault() != ProbeFault::None;
        if (r.id == "BT") s.bt = r.value;
        if (r.id == "ET") s.et = r.value;
        s.probes.push_back(r);
    }
    s.ror_bt = rorBt_;
    s.ror_et = rorEt_;
    for (int i = 0; i < kControlCount; ++i) {
        s.controls[i] = target_[i];
        s.actuators[i] = actual_[i];
        s.has_control[i] = roaster_.hasControl(static_cast<Control>(i));
    }
    if (machine_) s.tele = machine_->telemetry();
    if (beanLoc_ != BeanLocation::None) s.beans = beans_.summary();
    s.batch_g = batchG_;
    s.smoke = smoke_;
    s.chaff_rate_g_s = chaffRate_;
    s.pops_fc = popsFc_;
    s.pops_sc = popsSc_;
    s.alarms = lastAlarms_;
    s.powered = powered_;
    s.env = env_;
    s.last_event_seq = seq_;
    if (phase_ == RoastPhase::Roasting) {
        if (!marked_[MDry]) s.eta_dry_s = etaDry_;
        if (!marked_[MFcs]) s.eta_fc_s = etaFc_;
    }
    return s;
}

VisualState Simulation::visual() const {
    VisualState v;
    v.frame = frame_;
    v.t_session = t_;
    v.kind = roaster_.kind;
    v.roaster_id = roaster_.id;
    switch (roaster_.kind) {
        case RoasterKind::FluidBed:
            v.air_pattern = roaster_.fluid.air_pattern;
            v.geometry.chamber_diameter = static_cast<float>(roaster_.fluid.chamber_diameter_m);
            v.geometry.chamber_height = static_cast<float>(roaster_.fluid.chamber_height_m);
            break;
        case RoasterKind::Drum:
            v.geometry.chamber_diameter = static_cast<float>(roaster_.drum.drum_diameter_m);
            v.geometry.chamber_height = static_cast<float>(roaster_.drum.drum_length_m);
            break;
        case RoasterKind::StirPan:
            v.geometry.chamber_diameter = static_cast<float>(roaster_.pan.pan_diameter_m);
            v.geometry.chamber_height = static_cast<float>(roaster_.pan.dome_height_m);
            break;
    }
    v.bean_location = beanLoc_;
    if (machine_) {
        const MachineTelemetry& t = machine_->telemetry();
        v.agitation = static_cast<float>(t.agitation);
        v.mix_quality = static_cast<float>(t.mix_quality);
        v.angle_rad = static_cast<float>(t.angle_rad);
        v.fill = static_cast<float>(t.fill);
        v.element_glow = static_cast<float>(smoothstep(450, 900, t.element_C));
        v.alarms = t.alarms | lastAlarms_;
    }
    v.fan = static_cast<float>(actual_[static_cast<int>(Control::Fan)] / 100.0);
    v.heat = static_cast<float>(actual_[static_cast<int>(
                 roaster_.hasControl(Control::Heat) ? Control::Heat : Control::Voltage)] / 100.0);
    v.smoke = static_cast<float>(smoke_);
    v.chaff_airborne = static_cast<float>(chaffRate_);
    v.pops_fc = popsFc_;
    v.pops_sc = popsSc_;
    if (beanLoc_ != BeanLocation::None) {
        const Kinetics& k = beans_.kinetics();
        v.beans.reserve(beans_.parcels().size());
        for (const auto& p : beans_.parcels()) {
            VisualBean b;
            double rgb[3];
            BeanBatch::colorFor(p, beans_.spec(), k, rgb);
            for (int i = 0; i < 3; ++i) b.rgb[i] = static_cast<float>(rgb[i]);
            b.size_mm = static_cast<float>(p.d * 1000.0);
            b.oil = static_cast<float>(p.oil);
            b.temp_C = static_cast<float>(p.T_s);
            b.count = static_cast<float>(p.count);
            b.exposure = static_cast<float>(exposure(p, 0.3, 0.05));
            b.cracked = static_cast<float>(p.fc_frac);
            b.scorch = static_cast<float>(clamp01(p.scorch));
            v.beans.push_back(b);
        }
    }
    return v;
}

RoastReport Simulation::report() const {
    RoastReport r;
    if (chargeT_ < 0 || beanLoc_ == BeanLocation::None) return r;
    r.valid = true;
    r.roaster = roaster_.name;
    r.bean = beans_.spec().name;
    BeanSummary s = beans_.summary();
    r.batch_g = batchG_;
    r.out_g = s.mass_g;
    r.weight_loss_pct = s.weight_loss_pct;
    r.moisture_pct = s.moisture_pct;
    r.agtron_whole = s.agtron_whole;
    r.agtron_ground = s.agtron_ground;
    r.agtron_sd = s.agtron_sd;
    r.roast_level = roastLevelName(s.agtron_ground);
    r.scorch_pct = s.scorch_frac * 100.0;
    r.tipping_pct = s.tipping_frac * 100.0;
    r.ejected_pct = initialBeans_ > 0 ? 100.0 * (1.0 - s.beans / initialBeans_) : 0;

    for (const auto& e : events_) {
        if (e.seq <= chargeSeq_) continue;
        if (e.type == "TP" && r.tp_s < 0) { r.tp_s = e.t_roast; r.tp_bt = e.bt; }
        else if (e.type == "DRY" && r.dry_s < 0) r.dry_s = e.t_roast;
        else if (e.type == "FCs" && r.fcs_s < 0) { r.fcs_s = e.t_roast; r.fcs_bt = e.bt; }
        else if (e.type == "FCe" && r.fce_s < 0) r.fce_s = e.t_roast;
        else if (e.type == "SCs" && r.scs_s < 0) r.scs_s = e.t_roast;
        else if (e.type == "DROP" && r.drop_s < 0) { r.drop_s = e.t_roast; r.drop_bt = e.bt; }
    }
    double end = r.drop_s >= 0 ? r.drop_s : (t_ - chargeT_);
    if (r.fcs_s >= 0) {
        r.dev_time_s = std::max(0.0, end - r.fcs_s);
        r.dtr_pct = end > 0 ? 100.0 * r.dev_time_s / end : 0;
    }

    // RoR analysis from the 1 Hz history.
    double minRorAfterFc = 1e9, lowRun = 0, maxLowRun = 0;
    for (const auto& h : history_) {
        if (h.t_roast < 0 || h.t_roast > end) continue;
        if (r.tp_s >= 0 && h.t_roast > r.tp_s + 10) r.max_ror = std::max(r.max_ror, static_cast<double>(h.ror_bt));
        if (r.fcs_s >= 0 && std::fabs(h.t_roast - r.fcs_s) < 0.75) r.ror_at_fcs = h.ror_bt;
        if (std::fabs(h.t_roast - end) < 1.0) r.ror_at_drop = h.ror_bt;
        if (r.fcs_s >= 0 && h.t_roast >= r.fcs_s) {
            minRorAfterFc = std::min(minRorAfterFc, static_cast<double>(h.ror_bt));
            if (h.ror_bt < 1.0) {
                lowRun += 1.0;
                maxLowRun = std::max(maxLowRun, lowRun);
            } else {
                lowRun = 0;
            }
        }
    }
    if (r.fcs_s >= 0) {
        r.baked = maxLowRun >= 45 || (r.dev_time_s > 300 && r.ror_at_drop < 3.0);
        r.crashed = r.ror_at_fcs > 6 && minRorAfterFc < 0.35 * r.ror_at_fcs;
        r.flicked = minRorAfterFc < 1e8 && r.ror_at_drop > minRorAfterFc + 3.0 && r.dev_time_s > 30;
    }
    r.underdeveloped = r.fcs_s < 0 || r.dtr_pct < 12.0 || (s.agtron_ground - s.agtron_whole) > 14.0;

    auto note = [&](const std::string& n) { r.notes.push_back(n); };
    if (r.fcs_s < 0) note("Dropped before first crack: expect grassy, sour, underdeveloped cup.");
    else if (r.dtr_pct < 12) note("Very short development after first crack (DTR < 12%).");
    if (r.dtr_pct > 28) note("Long development (DTR > 28%): roasty, flattened acidity.");
    if (r.baked) note("RoR stalled after first crack: risk of baked, bready flavours.");
    if (r.crashed) note("RoR crashed after first crack.");
    if (r.flicked) note("RoR flicked up before drop.");
    if (r.scorch_pct > 5) note("Scorching on " + json::formatNumber(r.scorch_pct, 0) + "% of beans (contact surfaces too hot / poor agitation).");
    if (r.tipping_pct > 5) note("Tipping on " + json::formatNumber(r.tipping_pct, 0) + "% of beans (too much heat while beans were wet).");
    if (r.agtron_sd > 6) note("Uneven roast: colour spread " + json::formatNumber(r.agtron_sd, 1) + " Agtron points.");
    if (s.agtron_ground - s.agtron_whole > 12) note("Core much lighter than surface: likely underdeveloped inside.");
    if (r.ejected_pct > 0.5) note(json::formatNumber(r.ejected_pct, 1) + "% of beans were blown out of the chamber.");
    if (s.sc_frac > 0.5) note("Well into second crack: oily, bittersweet, origin character fading.");
    if (r.notes.empty()) note("Clean, even roast.");
    return r;
}

json::Value RoastReport::toJson() const {
    json::Value v = json::Value::object();
    v["valid"] = valid;
    if (!valid) return v;
    auto num = [](double x, int d) {
        if (!std::isfinite(x)) return json::Value();
        double p = std::pow(10.0, d);
        return json::Value(std::round(x * p) / p);
    };
    v["roaster"] = roaster;
    v["bean"] = bean;
    v["batch_g"] = num(batch_g, 1);
    v["out_g"] = num(out_g, 1);
    v["weight_loss_pct"] = num(weight_loss_pct, 2);
    v["moisture_pct"] = num(moisture_pct, 2);
    v["tp_s"] = num(tp_s, 0);
    v["tp_bt"] = num(tp_bt, 1);
    v["dry_s"] = num(dry_s, 0);
    v["fcs_s"] = num(fcs_s, 0);
    v["fcs_bt"] = num(fcs_bt, 1);
    v["fce_s"] = num(fce_s, 0);
    v["scs_s"] = num(scs_s, 0);
    v["drop_s"] = num(drop_s, 0);
    v["drop_bt"] = num(drop_bt, 1);
    v["dev_time_s"] = num(dev_time_s, 0);
    v["dtr_pct"] = num(dtr_pct, 1);
    v["agtron_whole"] = num(agtron_whole, 1);
    v["agtron_ground"] = num(agtron_ground, 1);
    v["agtron_sd"] = num(agtron_sd, 1);
    v["roast_level"] = roast_level;
    v["max_ror"] = num(max_ror, 1);
    v["ror_at_fcs"] = num(ror_at_fcs, 1);
    v["ror_at_drop"] = num(ror_at_drop, 1);
    v["scorch_pct"] = num(scorch_pct, 1);
    v["tipping_pct"] = num(tipping_pct, 1);
    v["ejected_pct"] = num(ejected_pct, 2);
    v["baked"] = baked;
    v["crashed"] = crashed;
    v["flicked"] = flicked;
    v["underdeveloped"] = underdeveloped;
    json::Value n = json::Value::array();
    for (const auto& s : notes) n.push(s);
    v["notes"] = n;
    return v;
}

}  // namespace roastsim
