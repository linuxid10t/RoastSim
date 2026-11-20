// The Simulation ties a machine model, a bean batch, probes and the roast
// timeline together. It is single-threaded and deterministic for a given seed;
// the host layer adds real-time pacing, locking and I/O around it.
#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "roastsim/bean.hpp"
#include "roastsim/json.hpp"
#include "roastsim/machine.hpp"
#include "roastsim/roaster_def.hpp"
#include "roastsim/sensors.hpp"
#include "roastsim/visual_state.hpp"

namespace roastsim {

struct SimEvent {
    std::uint64_t seq = 0;
    std::string type;    // CHARGE TP DRY FCs FCe SCs SCe DROP COOL ALARM NOTE
    std::string detail;  // alarm name, note text
    std::string origin;  // "sim" for physics-detected, else who asked (ui, web, artisan-ws, modbus, tc4...)
    double t_session = 0;
    double t_roast = 0;
    double bt = 0;
};

// One audible crack (first or second) from one bean, for sound and 3D effects.
struct CrackEvent {
    std::uint64_t seq = 0;
    double t = 0;            // session time of the crack
    bool second = false;
    float size_mm = 7.5f;    // see CrackPop
    float hardness = 1.0f;
    float moisture = 0.06f;
    float loudness = 1.0f;
    BeanLocation where = BeanLocation::Machine;  // tray cracks are heard in the open
};

struct ProbeReading {
    std::string id;
    ProbeLocation loc = ProbeLocation::BeanMass;
    double value = 0;
    bool fault = false;
};

struct HistorySample {
    float t = 0;        // session seconds
    float t_roast = 0;  // seconds since CHARGE (negative before)
    float bt = 0, et = 0, ror_bt = 0, ror_et = 0;
    float bean_true = 0;  // true mean bean temperature (not what the probe says)
    float heat = 0, fan = 0, drum = 0;
};

struct Snapshot {
    double t_session = 0;
    double t_roast = -1;  // < 0 when not charged
    RoastPhase phase = RoastPhase::Idle;
    BeanLocation bean_location = BeanLocation::None;
    std::string roaster_id, roaster_name, bean_id, bean_name;
    RoasterKind kind = RoasterKind::FluidBed;
    double bt = 0, et = 0, ror_bt = 0, ror_et = 0;
    std::vector<ProbeReading> probes;
    double controls[kControlCount] = {};   // requested %
    double actuators[kControlCount] = {};  // actual % after slew
    bool has_control[kControlCount] = {};
    MachineTelemetry tele;
    BeanSummary beans;
    double batch_g = 0;
    double smoke = 0;            // 0..1
    double chaff_rate_g_s = 0;
    std::uint64_t pops_fc = 0, pops_sc = 0;
    std::uint32_t alarms = 0;
    bool powered = false;
    Environment env;
    std::uint64_t last_event_seq = 0;
};

struct RoastReport {
    bool valid = false;
    std::string roaster, bean;
    double batch_g = 0, out_g = 0, weight_loss_pct = 0, moisture_pct = 0;
    double tp_s = -1, tp_bt = 0, dry_s = -1, fcs_s = -1, fcs_bt = 0, fce_s = -1, scs_s = -1;
    double drop_s = -1, drop_bt = 0, dev_time_s = 0, dtr_pct = 0;
    double agtron_whole = 0, agtron_ground = 0, agtron_sd = 0;
    std::string roast_level;
    double max_ror = 0, ror_at_fcs = 0, ror_at_drop = 0;
    double scorch_pct = 0, tipping_pct = 0;
    bool baked = false, underdeveloped = false, flicked = false, crashed = false;
    double ejected_pct = 0;
    std::vector<std::string> notes;
    json::Value toJson() const;
};

class Simulation {
public:
    static constexpr double kStep = 0.05;  // s, internal integration step

    Simulation();
    ~Simulation();

    // Loads (or replaces) the roaster and bean; resets to a cold, empty machine.
    void load(const RoasterDef& roaster, const BeanSpec& bean);
    void setBean(const BeanSpec& bean);   // for the next charge
    void setSeed(std::uint64_t seed);
    void setParcelCount(int n) { parcelCount_ = std::max(1, std::min(5000, n)); }
    int parcelCount() const { return parcelCount_; }

    void setEnvironment(const Environment& env) { env_ = env; }
    const Environment& environment() const { return env_; }

    // Controls in percent (clamped to the control's range).
    void setControl(Control c, double pct);
    double control(Control c) const { return target_[static_cast<int>(c)]; }

    // Master power switch: off drives every actuator toward 0 (the machine still
    // cools passively and the probes keep reading); the control settings are
    // kept for when it is switched on again. Defaults to off, also after load().
    bool powered() const { return powered_; }
    void setPower(bool on, const std::string& origin);

    bool charge(double batch_g, const std::string& origin, std::string* err = nullptr);
    bool drop(const std::string& origin, std::string* err = nullptr);
    bool coolEnd(const std::string& origin);
    void markEvent(const std::string& type, const std::string& origin);  // manual DRY/FCs/...
    void emptyMachine();   // discard beans, keep machine temperatures
    void coldReset();      // discard beans and cool the machine to ambient
    // Fast-forwards the empty machine with a simple controller until the ET
    // probe holds `target_C`. Returns simulated seconds used, or -1 on failure.
    double preheat(double target_C, double maxSeconds = 2400);
    void repair();
    bool setProbeFault(const std::string& probeId, ProbeFault f, double param = 0);

    void step(double dt);  // any dt; integrates in kStep slices

    Snapshot snapshot() const;
    VisualState visual() const;
    RoastReport report() const;
    const std::deque<HistorySample>& history() const { return history_; }
    std::uint64_t historyBase() const { return historyBase_; }  // index of history_.front()
    std::vector<SimEvent> eventsSince(std::uint64_t seq) const;
    // Individual crack sounds after `seq` (oldest first, at most `max`).
    std::vector<CrackEvent> cracksSince(std::uint64_t seq, std::size_t max = 4096) const;
    std::uint64_t crackSeq() const { return crackSeq_; }
    const std::vector<SimEvent>& events() const { return events_; }

    const RoasterDef& roaster() const { return roaster_; }
    const BeanSpec& bean() const { return bean_; }
    RoastPhase phase() const { return phase_; }
    double sessionTime() const { return t_; }
    double roastTime() const { return chargeT_ >= 0 ? t_ - chargeT_ : -1; }

private:
    void stepOnce(double dt);
    void stepTray(double dt);
    void updateProbes(double dt);
    void sampleHistory();
    void detectEvents();
    void addEvent(const std::string& type, const std::string& origin, const std::string& detail = "");
    double rorFrom(const std::deque<std::pair<double, double>>& buf) const;
    int probeIndex(const char* id) const;

    RoasterDef roaster_;
    BeanSpec bean_;
    Environment env_;
    Rng rng_;
    std::unique_ptr<MachineModel> machine_;
    BeanBatch beans_;
    BeanLocation beanLoc_ = BeanLocation::None;
    std::vector<Probe> probes_;
    int parcelCount_ = 200;

    double target_[kControlCount] = {};
    double actual_[kControlCount] = {};
    bool powered_ = false;  // master switch: off forces the actuators to 0

    double t_ = 0;
    double stepCarry_ = 0;
    double chargeT_ = -1, dropT_ = -1;
    double batchG_ = 0;
    double initialBeans_ = 0;
    RoastPhase phase_ = RoastPhase::Idle;
    std::uint64_t frame_ = 0;

    // Event bookkeeping
    std::vector<SimEvent> events_;
    std::uint64_t seq_ = 0;
    bool marked_[16] = {};
    double tpMin_ = 1e9, tpMinT_ = -1;
    std::uint32_t lastAlarms_ = 0;  // latched alarm bits
    double alarmSeen_[32] = {};

    // Rates
    std::uint64_t popsFc_ = 0, popsSc_ = 0;
    std::deque<CrackEvent> cracks_;
    std::uint64_t crackSeq_ = 0;
    double smoke_ = 0, chaffRate_ = 0;
    double organicRate_ = 0;  // kg/s smoothed

    // RoR buffers: (time, reading) pairs over the last ~40 s, sampled at 2 Hz
    std::deque<std::pair<double, double>> btBuf_, etBuf_;
    double rorBt_ = 0, rorEt_ = 0;
    double nextRorSample_ = 0, nextHistory_ = 0;

    std::deque<HistorySample> history_;
    std::uint64_t historyBase_ = 0;
};

const char* roastLevelName(double agtronGround);

}  // namespace roastsim
